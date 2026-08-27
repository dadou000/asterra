#include "voronoi_precipitation.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace asterra::weather {

namespace {
constexpr double WATER_CONSERVATION_TOL = 2.0e-11;
constexpr double ENERGY_CONSERVATION_TOL = 2.0e-11;

double relative_error(double after, double before) {
	return std::abs(after - before) / std::max(std::abs(before), 1.0);
}

double relative_error_scaled(double after, double before, double scale) {
	return std::abs(after - before) / std::max(std::abs(scale), 1.0);
}

double column_thermodynamic_energy_j_m2(
		const VoronoiDryTransport::State &state,
		const VoronoiMoistThermodynamics::ThermodynamicDiagnostics &thermo,
		const VoronoiMoistThermodynamics::TracerIndices &indices,
		int cell, int cells) {
	const auto &vapor = state.tracer_mass_kg_m2[static_cast<size_t>(indices.vapor)];
	const auto &cloud_ice = state.tracer_mass_kg_m2[static_cast<size_t>(indices.cloud_ice)];
	const auto &snow = state.tracer_mass_kg_m2[static_cast<size_t>(indices.snow)];
	long double total = 0.0L;
	for (int k = 0; k < VoronoiDryTransport::LEVELS; ++k) {
		const size_t i = static_cast<size_t>(k * cells + cell);
		total += static_cast<long double>(VoronoiMoistThermodynamics::CP_DRY)
			* static_cast<long double>(thermo.temperature_k[i])
			* static_cast<long double>(state.layer_mass_kg_m2[i]);
		total += static_cast<long double>(VoronoiMoistThermodynamics::LV0_J_KG)
			* static_cast<long double>(vapor[i]);
		total -= static_cast<long double>(VoronoiMoistThermodynamics::LF0_J_KG)
			* static_cast<long double>(cloud_ice[i] + snow[i]);
	}
	return static_cast<double>(total);
}

} // namespace

VoronoiPrecipitation::VoronoiPrecipitation(const VoronoiDryTransport &transport,
		TracerIndices indices)
	: transport_(&transport), indices_(indices) {
	VoronoiMoistThermodynamics validate(transport, indices);
	(void)validate;
	if (transport.grid().cell_count() <= 0) {
		throw std::invalid_argument("Precipitation requires a built weather grid");
	}
}

void VoronoiPrecipitation::ensure_precipitation_tracers(State &state) const {
	VoronoiMoistThermodynamics moist(*transport_, indices_);
	moist.ensure_water_tracers(state);
}

void VoronoiPrecipitation::validate_state_surface(
		const State &state, const SurfaceState &surface) const {
	VoronoiMoistThermodynamics moist(*transport_, indices_);
	(void)moist.diagnose_thermodynamics(state);
	const size_t cells = static_cast<size_t>(transport_->grid().cell_count());
	if (surface.water_kg_m2.size() != cells
			|| surface.ice_kg_m2.size() != cells
			|| surface.energy_j_m2.size() != cells) {
		throw std::invalid_argument("Precipitation surface reservoir arrays have wrong size");
	}
	for (double water : surface.water_kg_m2) {
		if (!(water >= 0.0) || !std::isfinite(water)) {
			throw std::runtime_error("Precipitation encountered invalid liquid surface water");
		}
	}
	for (double ice : surface.ice_kg_m2) {
		if (!(ice >= 0.0) || !std::isfinite(ice)) {
			throw std::runtime_error("Precipitation encountered invalid frozen surface water");
		}
	}
	for (double energy : surface.energy_j_m2) {
		if (!std::isfinite(energy)) {
			throw std::runtime_error("Precipitation encountered non-finite surface energy");
		}
	}
}

double VoronoiPrecipitation::total_atmospheric_water_kg(const State &state) const {
	VoronoiSurfaceExchange exchange(*transport_, indices_);
	return exchange.total_atmospheric_water_kg(state);
}

double VoronoiPrecipitation::total_rain_kg(const State &state) const {
	return transport_->total_tracer_mass_kg(state, indices_.rain);
}

double VoronoiPrecipitation::total_snow_kg(const State &state) const {
	return transport_->total_tracer_mass_kg(state, indices_.snow);
}

VoronoiPrecipitation::Tendencies VoronoiPrecipitation::compute_tendencies(
		const State &state, double rain_fall_speed_mps,
		double snow_fall_speed_mps) const {
	if (!(rain_fall_speed_mps >= 0.0) || !std::isfinite(rain_fall_speed_mps)
			|| !(snow_fall_speed_mps >= 0.0) || !std::isfinite(snow_fall_speed_mps)) {
		throw std::invalid_argument("Precipitation fall speeds must be finite and non-negative");
	}
	VoronoiMoistHydrostatic hydrostatic(*transport_, indices_);
	const auto hydro = hydrostatic.diagnose(state);
	const int cells = transport_->grid().cell_count();
	const size_t scalars = static_cast<size_t>(LEVELS) * cells;
	Tendencies t;
	t.rain_dt.assign(scalars, 0.0);
	t.snow_dt.assign(scalars, 0.0);
	t.surface_rain_dt.assign(static_cast<size_t>(cells), 0.0);
	t.surface_snow_dt.assign(static_cast<size_t>(cells), 0.0);

	const auto &rain = state.tracer_mass_kg_m2[static_cast<size_t>(indices_.rain)];
	const auto &snow = state.tracer_mass_kg_m2[static_cast<size_t>(indices_.snow)];
	const double gravity = transport_->gravity_mps2();

	auto sediment_species = [&](const std::vector<double> &field, double fall_speed,
			std::vector<double> &field_dt, std::vector<double> &surface_dt) {
		if (fall_speed == 0.0) return;
		for (int c = 0; c < cells; ++c) {
			for (int k = 0; k < LEVELS; ++k) {
				const size_t i = static_cast<size_t>(scalar_index(k, c));
				const double phi_lower = hydro.interface_geopotential[
					static_cast<size_t>(interface_index(k, c))];
				const double phi_upper = hydro.interface_geopotential[
					static_cast<size_t>(interface_index(k + 1, c))];
				const double dz = (phi_upper - phi_lower) / gravity;
				if (!(dz > 0.0) || !std::isfinite(dz)) {
					throw std::runtime_error("Precipitation diagnosed invalid layer thickness");
				}
				const double downward_flux = fall_speed * field[i] / dz;
				if (!(downward_flux >= 0.0) || !std::isfinite(downward_flux)) {
					throw std::runtime_error("Precipitation produced invalid downward mass flux");
				}
				field_dt[i] -= downward_flux;
				if (k == 0) {
					surface_dt[static_cast<size_t>(c)] += downward_flux;
				} else {
					field_dt[static_cast<size_t>(scalar_index(k - 1, c))] += downward_flux;
				}
			}
		}
	};

	sediment_species(rain, rain_fall_speed_mps, t.rain_dt, t.surface_rain_dt);
	sediment_species(snow, snow_fall_speed_mps, t.snow_dt, t.surface_snow_dt);
	return t;
}

double VoronoiPrecipitation::max_courant(const State &state, double dt_s,
		double rain_fall_speed_mps, double snow_fall_speed_mps) const {
	if (!(dt_s >= 0.0) || !std::isfinite(dt_s)) {
		throw std::invalid_argument("Precipitation CFL timestep must be finite and non-negative");
	}
	if (!(rain_fall_speed_mps >= 0.0) || !std::isfinite(rain_fall_speed_mps)
			|| !(snow_fall_speed_mps >= 0.0) || !std::isfinite(snow_fall_speed_mps)) {
		throw std::invalid_argument("Precipitation fall speeds must be finite and non-negative");
	}
	VoronoiMoistHydrostatic hydrostatic(*transport_, indices_);
	const auto hydro = hydrostatic.diagnose(state);
	const auto &rain = state.tracer_mass_kg_m2[static_cast<size_t>(indices_.rain)];
	const auto &snow = state.tracer_mass_kg_m2[static_cast<size_t>(indices_.snow)];
	const int cells = transport_->grid().cell_count();
	const double gravity = transport_->gravity_mps2();

	const bool rain_active = std::any_of(rain.begin(), rain.end(), [](double m) { return m > 0.0; });
	const bool snow_active = std::any_of(snow.begin(), snow.end(), [](double m) { return m > 0.0; });
	if ((!rain_active || rain_fall_speed_mps == 0.0)
			&& (!snow_active || snow_fall_speed_mps == 0.0)) return 0.0;

	double maximum = 0.0;
	for (int c = 0; c < cells; ++c) {
		for (int k = 0; k < LEVELS; ++k) {
			const double dz = (hydro.interface_geopotential[
				static_cast<size_t>(interface_index(k + 1, c))]
				- hydro.interface_geopotential[
					static_cast<size_t>(interface_index(k, c))]) / gravity;
			if (!(dz > 0.0) || !std::isfinite(dz)) {
				throw std::runtime_error("Precipitation CFL diagnosed invalid layer thickness");
			}
			if (rain_active && rain_fall_speed_mps > 0.0) {
				maximum = std::max(maximum, dt_s * rain_fall_speed_mps / dz);
			}
			if (snow_active && snow_fall_speed_mps > 0.0) {
				maximum = std::max(maximum, dt_s * snow_fall_speed_mps / dz);
			}
		}
	}
	return maximum;
}

double VoronoiPrecipitation::stable_dt(const State &state, double target_cfl,
		double maximum_dt_s, double rain_fall_speed_mps,
		double snow_fall_speed_mps) const {
	if (!(target_cfl > 0.0) || target_cfl > 0.9 || !std::isfinite(target_cfl)) {
		throw std::invalid_argument("Precipitation target CFL must be finite and in (0,0.9]");
	}
	if (!(maximum_dt_s > 0.0) || !std::isfinite(maximum_dt_s)) {
		throw std::invalid_argument("Precipitation maximum timestep must be finite and positive");
	}
	const double unit = max_courant(state, 1.0,
		rain_fall_speed_mps, snow_fall_speed_mps);
	if (unit == 0.0) return maximum_dt_s;
	if (!(unit > 0.0) || !std::isfinite(unit)) {
		throw std::runtime_error("Precipitation state has invalid sedimentation speed");
	}
	return std::min(maximum_dt_s, target_cfl / unit);
}

bool VoronoiPrecipitation::euler_stage(const State &input,
		const SurfaceState &surface_input, State &output,
		SurfaceState &surface_output, double dt_s,
		double rain_fall_speed_mps, double snow_fall_speed_mps) const {
	try {
		const Tendencies t = compute_tendencies(input,
			rain_fall_speed_mps, snow_fall_speed_mps);
		VoronoiMoistThermodynamics moist(*transport_, indices_);
		const auto thermo_before = moist.diagnose_thermodynamics(input);
		output = input;
		surface_output = surface_input;
		auto &rain = output.tracer_mass_kg_m2[static_cast<size_t>(indices_.rain)];
		auto &snow = output.tracer_mass_kg_m2[static_cast<size_t>(indices_.snow)];
		for (size_t i = 0; i < rain.size(); ++i) {
			rain[i] += dt_s * t.rain_dt[i];
			snow[i] += dt_s * t.snow_dt[i];
			if (!(rain[i] >= 0.0) || !(snow[i] >= 0.0)
					|| !std::isfinite(rain[i]) || !std::isfinite(snow[i])) return false;
		}
		for (size_t c = 0; c < surface_output.water_kg_m2.size(); ++c) {
			surface_output.water_kg_m2[c] += dt_s * t.surface_rain_dt[c];
			surface_output.ice_kg_m2[c] += dt_s * t.surface_snow_dt[c];
			if (!(surface_output.water_kg_m2[c] >= 0.0)
					|| !(surface_output.ice_kg_m2[c] >= 0.0)
					|| !std::isfinite(surface_output.water_kg_m2[c])
					|| !std::isfinite(surface_output.ice_kg_m2[c])) return false;
		}

		// Removing hydrometeor loading changes full pressure and therefore T at
		// fixed theta. Transfer the exact local atmospheric source-energy change
		// to the surface rather than accounting only for snow's fusion reference.
		const auto thermo_after = moist.diagnose_thermodynamics(output);
		const int cells = transport_->grid().cell_count();
		for (int c = 0; c < cells; ++c) {
			const double before = column_thermodynamic_energy_j_m2(
				input, thermo_before, indices_, c, cells);
			const double after = column_thermodynamic_energy_j_m2(
				output, thermo_after, indices_, c, cells);
			surface_output.energy_j_m2[static_cast<size_t>(c)]
				= surface_input.energy_j_m2[static_cast<size_t>(c)] - (after - before);
			if (!std::isfinite(surface_output.energy_j_m2[static_cast<size_t>(c)])) return false;
		}
		return true;
	} catch (const std::exception &) {
		return false;
	}
}

bool VoronoiPrecipitation::ssprk3_attempt(const State &initial,
		const SurfaceState &surface_initial, State &candidate,
		SurfaceState &surface_candidate, double dt_s,
		double rain_fall_speed_mps, double snow_fall_speed_mps) const {
	State s1;
	SurfaceState surface1;
	if (!euler_stage(initial, surface_initial, s1, surface1, dt_s,
			rain_fall_speed_mps, snow_fall_speed_mps)) return false;
	State euler2;
	SurfaceState surface_euler2;
	if (!euler_stage(s1, surface1, euler2, surface_euler2, dt_s,
			rain_fall_speed_mps, snow_fall_speed_mps)) return false;

	State s2 = initial;
	SurfaceState surface2 = surface_initial;
	for (int tracer : {indices_.rain, indices_.snow}) {
		auto &dst = s2.tracer_mass_kg_m2[static_cast<size_t>(tracer)];
		const auto &a = initial.tracer_mass_kg_m2[static_cast<size_t>(tracer)];
		const auto &b = euler2.tracer_mass_kg_m2[static_cast<size_t>(tracer)];
		for (size_t i = 0; i < dst.size(); ++i) dst[i] = 0.75 * a[i] + 0.25 * b[i];
	}
	for (size_t c = 0; c < surface2.water_kg_m2.size(); ++c) {
		surface2.water_kg_m2[c] = 0.75 * surface_initial.water_kg_m2[c]
			+ 0.25 * surface_euler2.water_kg_m2[c];
		surface2.ice_kg_m2[c] = 0.75 * surface_initial.ice_kg_m2[c]
			+ 0.25 * surface_euler2.ice_kg_m2[c];
		surface2.energy_j_m2[c] = 0.75 * surface_initial.energy_j_m2[c]
			+ 0.25 * surface_euler2.energy_j_m2[c];
	}
	State euler3;
	SurfaceState surface_euler3;
	if (!euler_stage(s2, surface2, euler3, surface_euler3, dt_s,
			rain_fall_speed_mps, snow_fall_speed_mps)) return false;

	candidate = initial;
	surface_candidate = surface_initial;
	for (int tracer : {indices_.rain, indices_.snow}) {
		auto &dst = candidate.tracer_mass_kg_m2[static_cast<size_t>(tracer)];
		const auto &a = initial.tracer_mass_kg_m2[static_cast<size_t>(tracer)];
		const auto &b = euler3.tracer_mass_kg_m2[static_cast<size_t>(tracer)];
		for (size_t i = 0; i < dst.size(); ++i) dst[i] = (1.0 / 3.0) * a[i] + (2.0 / 3.0) * b[i];
	}
	for (size_t c = 0; c < surface_candidate.water_kg_m2.size(); ++c) {
		surface_candidate.water_kg_m2[c] = (1.0 / 3.0) * surface_initial.water_kg_m2[c]
			+ (2.0 / 3.0) * surface_euler3.water_kg_m2[c];
		surface_candidate.ice_kg_m2[c] = (1.0 / 3.0) * surface_initial.ice_kg_m2[c]
			+ (2.0 / 3.0) * surface_euler3.ice_kg_m2[c];
		surface_candidate.energy_j_m2[c] = (1.0 / 3.0) * surface_initial.energy_j_m2[c]
			+ (2.0 / 3.0) * surface_euler3.energy_j_m2[c];
	}
	for (int tracer : {indices_.rain, indices_.snow}) {
		for (double mass : candidate.tracer_mass_kg_m2[static_cast<size_t>(tracer)]) {
			if (!(mass >= 0.0) || !std::isfinite(mass)) return false;
		}
	}
	for (size_t c = 0; c < surface_candidate.water_kg_m2.size(); ++c) {
		if (!(surface_candidate.water_kg_m2[c] >= 0.0)
				|| !(surface_candidate.ice_kg_m2[c] >= 0.0)
				|| !std::isfinite(surface_candidate.water_kg_m2[c])
				|| !std::isfinite(surface_candidate.ice_kg_m2[c])
				|| !std::isfinite(surface_candidate.energy_j_m2[c])) return false;
	}
	return true;
}

VoronoiPrecipitation::Diagnostics VoronoiPrecipitation::step(
		State &state, SurfaceState &surface, double requested_dt_s,
		double rain_fall_speed_mps, double snow_fall_speed_mps,
		double target_cfl, int max_retries) const {
	validate_state_surface(state, surface);
	if (!(requested_dt_s > 0.0) || !std::isfinite(requested_dt_s)) {
		throw std::invalid_argument("Precipitation timestep must be finite and positive");
	}
	if (!(target_cfl > 0.0) || target_cfl > 0.9 || !std::isfinite(target_cfl)) {
		throw std::invalid_argument("Precipitation target CFL must be finite and in (0,0.9]");
	}
	if (max_retries < 0) throw std::invalid_argument("Precipitation retry count cannot be negative");

	VoronoiSurfaceExchange exchange(*transport_, indices_);
	Diagnostics d;
	d.requested_dt_s = requested_dt_s;
	d.atmosphere_water_before_kg = total_atmospheric_water_kg(state);
	d.surface_water_before_kg = exchange.total_surface_water_kg(surface);
	d.atmosphere_thermo_before_j = exchange.atmospheric_thermodynamic_energy_j(state);
	d.surface_energy_before_j = exchange.total_surface_energy_j(surface);
	d.rain_before_kg = total_rain_kg(state);
	d.snow_before_kg = total_snow_kg(state);
	const State original = state;
	const SurfaceState surface_original = surface;
	const int cells = transport_->grid().cell_count();

	bool any_precip = false;
	for (double m : state.tracer_mass_kg_m2[static_cast<size_t>(indices_.rain)]) {
		if (m != 0.0) { any_precip = true; break; }
	}
	if (!any_precip) {
		for (double m : state.tracer_mass_kg_m2[static_cast<size_t>(indices_.snow)]) {
			if (m != 0.0) { any_precip = true; break; }
		}
	}
	if (!any_precip || (rain_fall_speed_mps == 0.0 && snow_fall_speed_mps == 0.0)) {
		d.accepted_dt_s = requested_dt_s;
		d.substeps = 1;
		d.atmosphere_water_after_kg = d.atmosphere_water_before_kg;
		d.surface_water_after_kg = d.surface_water_before_kg;
		d.atmosphere_thermo_after_j = d.atmosphere_thermo_before_j;
		d.surface_energy_after_j = d.surface_energy_before_j;
		d.rain_after_kg = d.rain_before_kg;
		d.snow_after_kg = d.snow_before_kg;
		d.min_rain_kg_m2 = 0.0;
		d.min_snow_kg_m2 = 0.0;
		d.surface_rain_flux_kg_m2_s.assign(static_cast<size_t>(cells), 0.0);
		d.surface_snow_flux_kg_m2_s.assign(static_cast<size_t>(cells), 0.0);
		return d;
	}

	double attempt_dt = stable_dt(original, target_cfl, requested_dt_s,
		rain_fall_speed_mps, snow_fall_speed_mps);
	for (int attempt = 0; attempt <= max_retries; ++attempt) {
		State candidate;
		SurfaceState surface_candidate;
		if (ssprk3_attempt(original, surface_original, candidate, surface_candidate,
				attempt_dt, rain_fall_speed_mps, snow_fall_speed_mps)) {
			const double atmosphere_after = total_atmospheric_water_kg(candidate);
			const double surface_after = exchange.total_surface_water_kg(surface_candidate);
			const double atmosphere_energy_after = exchange.atmospheric_thermodynamic_energy_j(candidate);
			const double surface_energy_after = exchange.total_surface_energy_j(surface_candidate);
			const double system_before = d.atmosphere_water_before_kg + d.surface_water_before_kg;
			const double system_after = atmosphere_after + surface_after;
			const double energy_before = d.atmosphere_thermo_before_j + d.surface_energy_before_j;
			const double energy_after = atmosphere_energy_after + surface_energy_after;
			const double water_error = relative_error(system_after, system_before);
			const double energy_scale = std::abs(d.atmosphere_thermo_before_j)
				+ std::abs(d.surface_energy_before_j);
			const double energy_error = relative_error_scaled(energy_after, energy_before, energy_scale);
			if (std::isfinite(water_error) && water_error <= WATER_CONSERVATION_TOL
					&& std::isfinite(energy_error) && energy_error <= ENERGY_CONSERVATION_TOL) {
				state = std::move(candidate);
				surface = std::move(surface_candidate);
				d.accepted_dt_s = attempt_dt;
				d.substeps = 1;
				d.max_courant = max_courant(original, attempt_dt,
					rain_fall_speed_mps, snow_fall_speed_mps);
				d.atmosphere_water_after_kg = atmosphere_after;
				d.surface_water_after_kg = surface_after;
				d.relative_system_water_error = water_error;
				d.atmosphere_thermo_after_j = atmosphere_energy_after;
				d.surface_energy_after_j = surface_energy_after;
				d.relative_system_energy_error = energy_error;
				break;
			}
		}
		++d.rejected_steps;
		attempt_dt *= 0.5;
		if (!(attempt_dt > std::numeric_limits<double>::min())) break;
	}

	if (!(d.accepted_dt_s > 0.0)) {
		state = original;
		surface = surface_original;
		d.atmosphere_water_after_kg = d.atmosphere_water_before_kg;
		d.surface_water_after_kg = d.surface_water_before_kg;
		d.atmosphere_thermo_after_j = d.atmosphere_thermo_before_j;
		d.surface_energy_after_j = d.surface_energy_before_j;
		d.rain_after_kg = d.rain_before_kg;
		d.snow_after_kg = d.snow_before_kg;
		d.surface_rain_flux_kg_m2_s.assign(static_cast<size_t>(cells), 0.0);
		d.surface_snow_flux_kg_m2_s.assign(static_cast<size_t>(cells), 0.0);
		return d;
	}

	d.rain_after_kg = total_rain_kg(state);
	d.snow_after_kg = total_snow_kg(state);
	d.deposited_rain_kg = std::max(0.0, d.rain_before_kg - d.rain_after_kg);
	d.deposited_snow_kg = std::max(0.0, d.snow_before_kg - d.snow_after_kg);
	d.min_rain_kg_m2 = std::numeric_limits<double>::infinity();
	d.min_snow_kg_m2 = std::numeric_limits<double>::infinity();
	const auto &rain_before = original.tracer_mass_kg_m2[static_cast<size_t>(indices_.rain)];
	const auto &snow_before = original.tracer_mass_kg_m2[static_cast<size_t>(indices_.snow)];
	const auto &rain_after = state.tracer_mass_kg_m2[static_cast<size_t>(indices_.rain)];
	const auto &snow_after = state.tracer_mass_kg_m2[static_cast<size_t>(indices_.snow)];
	d.surface_rain_flux_kg_m2_s.assign(static_cast<size_t>(cells), 0.0);
	d.surface_snow_flux_kg_m2_s.assign(static_cast<size_t>(cells), 0.0);
	for (int c = 0; c < cells; ++c) {
		double rain_column_before = 0.0;
		double rain_column_after = 0.0;
		double snow_column_before = 0.0;
		double snow_column_after = 0.0;
		for (int k = 0; k < LEVELS; ++k) {
			const size_t i = static_cast<size_t>(scalar_index(k, c));
			rain_column_before += rain_before[i];
			rain_column_after += rain_after[i];
			snow_column_before += snow_before[i];
			snow_column_after += snow_after[i];
			d.min_rain_kg_m2 = std::min(d.min_rain_kg_m2, rain_after[i]);
			d.min_snow_kg_m2 = std::min(d.min_snow_kg_m2, snow_after[i]);
		}
		const double rain_flux = std::max(0.0,
			(rain_column_before - rain_column_after) / d.accepted_dt_s);
		const double snow_flux = std::max(0.0,
			(snow_column_before - snow_column_after) / d.accepted_dt_s);
		d.surface_rain_flux_kg_m2_s[static_cast<size_t>(c)] = rain_flux;
		d.surface_snow_flux_kg_m2_s[static_cast<size_t>(c)] = snow_flux;
		d.max_surface_precip_flux_kg_m2_s = std::max(
			d.max_surface_precip_flux_kg_m2_s, rain_flux + snow_flux);
	}
	return d;
}

VoronoiPrecipitation::Diagnostics VoronoiPrecipitation::advance_full_interval(
		State &state, SurfaceState &surface, double interval_dt_s,
		double rain_fall_speed_mps, double snow_fall_speed_mps,
		double target_cfl, int max_retries_per_substep, int max_substeps) const {
	validate_state_surface(state, surface);
	if (!(interval_dt_s > 0.0) || !std::isfinite(interval_dt_s)) {
		throw std::invalid_argument("Precipitation full interval must be finite and positive");
	}
	if (max_substeps <= 0) {
		throw std::invalid_argument("Precipitation max_substeps must be positive");
	}

	const State original = state;
	const SurfaceState surface_original = surface;
	VoronoiSurfaceExchange exchange(*transport_, indices_);
	Diagnostics total;
	total.requested_dt_s = interval_dt_s;
	total.atmosphere_water_before_kg = total_atmospheric_water_kg(state);
	total.surface_water_before_kg = exchange.total_surface_water_kg(surface);
	total.atmosphere_thermo_before_j = exchange.atmospheric_thermodynamic_energy_j(state);
	total.surface_energy_before_j = exchange.total_surface_energy_j(surface);
	total.rain_before_kg = total_rain_kg(state);
	total.snow_before_kg = total_snow_kg(state);
	const int cells = transport_->grid().cell_count();
	std::vector<double> rain_depth(static_cast<size_t>(cells), 0.0);
	std::vector<double> snow_depth(static_cast<size_t>(cells), 0.0);

	double remaining = interval_dt_s;
	const double time_tol = 1.0e-12 * std::max(1.0, interval_dt_s);
	while (remaining > time_tol) {
		if (total.substeps >= max_substeps) {
			state = original;
			surface = surface_original;
			total.accepted_dt_s = 0.0;
			return total;
		}
		const Diagnostics d = step(state, surface, remaining,
			rain_fall_speed_mps, snow_fall_speed_mps,
			target_cfl, max_retries_per_substep);
		total.rejected_steps += d.rejected_steps;
		if (!(d.accepted_dt_s > 0.0) || !std::isfinite(d.accepted_dt_s)
				|| d.accepted_dt_s > remaining + time_tol) {
			state = original;
			surface = surface_original;
			total.accepted_dt_s = 0.0;
			return total;
		}
		for (int c = 0; c < cells; ++c) {
			rain_depth[static_cast<size_t>(c)] +=
				d.surface_rain_flux_kg_m2_s[static_cast<size_t>(c)] * d.accepted_dt_s;
			snow_depth[static_cast<size_t>(c)] +=
				d.surface_snow_flux_kg_m2_s[static_cast<size_t>(c)] * d.accepted_dt_s;
		}
		total.deposited_rain_kg += d.deposited_rain_kg;
		total.deposited_snow_kg += d.deposited_snow_kg;
		total.max_courant = std::max(total.max_courant, d.max_courant);
		++total.substeps;
		remaining -= d.accepted_dt_s;
		if (remaining <= time_tol) remaining = 0.0;
	}

	total.accepted_dt_s = interval_dt_s;
	total.atmosphere_water_after_kg = total_atmospheric_water_kg(state);
	total.surface_water_after_kg = exchange.total_surface_water_kg(surface);
	total.atmosphere_thermo_after_j = exchange.atmospheric_thermodynamic_energy_j(state);
	total.surface_energy_after_j = exchange.total_surface_energy_j(surface);
	total.rain_after_kg = total_rain_kg(state);
	total.snow_after_kg = total_snow_kg(state);
	const double water_before = total.atmosphere_water_before_kg + total.surface_water_before_kg;
	const double water_after = total.atmosphere_water_after_kg + total.surface_water_after_kg;
	total.relative_system_water_error = relative_error(water_after, water_before);
	const double energy_before = total.atmosphere_thermo_before_j + total.surface_energy_before_j;
	const double energy_after = total.atmosphere_thermo_after_j + total.surface_energy_after_j;
	const double energy_scale = std::abs(total.atmosphere_thermo_before_j)
		+ std::abs(total.surface_energy_before_j);
	total.relative_system_energy_error = relative_error_scaled(
		energy_after, energy_before, energy_scale);

	if (!(total.relative_system_water_error <= WATER_CONSERVATION_TOL)
			|| !(total.relative_system_energy_error <= ENERGY_CONSERVATION_TOL)) {
		state = original;
		surface = surface_original;
		total.accepted_dt_s = 0.0;
		return total;
	}

	total.min_rain_kg_m2 = std::numeric_limits<double>::infinity();
	total.min_snow_kg_m2 = std::numeric_limits<double>::infinity();
	for (double m : state.tracer_mass_kg_m2[static_cast<size_t>(indices_.rain)]) {
		total.min_rain_kg_m2 = std::min(total.min_rain_kg_m2, m);
	}
	for (double m : state.tracer_mass_kg_m2[static_cast<size_t>(indices_.snow)]) {
		total.min_snow_kg_m2 = std::min(total.min_snow_kg_m2, m);
	}
	total.surface_rain_flux_kg_m2_s.resize(static_cast<size_t>(cells));
	total.surface_snow_flux_kg_m2_s.resize(static_cast<size_t>(cells));
	for (int c = 0; c < cells; ++c) {
		const double rain_flux = rain_depth[static_cast<size_t>(c)] / interval_dt_s;
		const double snow_flux = snow_depth[static_cast<size_t>(c)] / interval_dt_s;
		total.surface_rain_flux_kg_m2_s[static_cast<size_t>(c)] = rain_flux;
		total.surface_snow_flux_kg_m2_s[static_cast<size_t>(c)] = snow_flux;
		total.max_surface_precip_flux_kg_m2_s = std::max(
			total.max_surface_precip_flux_kg_m2_s, rain_flux + snow_flux);
	}
	return total;
}

} // namespace asterra::weather

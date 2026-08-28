#include "voronoi_dry_core.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace asterra::weather {

namespace {
constexpr double CORE_CONSERVATION_REJECT_TOL = 3.0e-10;
constexpr double COORDINATE_ALREADY_ALIGNED_TOL = 5.0e-15;
constexpr double DIVERGENCE_GAMMA = 1.4;
constexpr double MAX_DAMPING_SUBSTEP_COURANT = 0.06;
constexpr double QUIET_DIVERGENCE_S1 = 1.0e-12;
constexpr double ANTIDIFFUSIVE_REL_TOL = 2.0e-10;
constexpr int MAX_DAMPING_SUBSTEPS = 64;

double relative_error(double after, double before) {
	return std::abs(after - before) / std::max(std::abs(before), 1.0);
}
} // namespace

VoronoiDryCore::VoronoiDryCore(const GeodesicVoronoiGrid &grid,
		double gravity_mps2, double scale_height_m, double top_pressure_pa,
		double rotation_rate_rad_s, Vec3d rotation_axis)
	: grid_(&grid),
	  dynamics_(grid, gravity_mps2, scale_height_m, top_pressure_pa,
		rotation_rate_rad_s, rotation_axis),
	  vertical_(grid, gravity_mps2, scale_height_m, top_pressure_pa) {
	if (grid.cell_count() <= 0 || grid.edge_count() <= 0) {
		throw std::invalid_argument("VoronoiDryCore requires a built geodesic grid");
	}
}

void VoronoiDryCore::set_divergence_damping_strength(double strength) {
	if (!(strength >= 0.0) || strength > MAX_DIVERGENCE_DAMPING_STRENGTH
			|| !std::isfinite(strength)) {
		throw std::invalid_argument(
			"Divergence damping strength must be finite and in [0, 0.35]");
	}
	divergence_damping_strength_ = strength;
}

std::vector<double> VoronoiDryCore::horizontal_divergence_s1(
		const State &state, int level) const {
	if (level < 0 || level >= LEVELS) {
		throw std::out_of_range("Dry-core divergence level is out of range");
	}
	const size_t expected_edges = static_cast<size_t>(LEVELS)
		* static_cast<size_t>(grid_->edge_count());
	if (state.edge_normal_mps.size() != expected_edges) {
		throw std::invalid_argument("Dry-core edge-wind array has wrong size for divergence");
	}

	std::vector<double> divergence(static_cast<size_t>(grid_->cell_count()), 0.0);
	for (int e = 0; e < grid_->edge_count(); ++e) {
		const auto &edge = grid_->edge(e);
		const double u = state.edge_normal_mps[
			static_cast<size_t>(level * grid_->edge_count() + e)];
		if (!std::isfinite(u)) {
			throw std::runtime_error("Dry-core divergence received non-finite wind");
		}
		const double volume_flux_m2_s = edge.edge_length_m * u;
		divergence[static_cast<size_t>(edge.cell_a)] += volume_flux_m2_s
			/ grid_->cell(edge.cell_a).area_m2;
		divergence[static_cast<size_t>(edge.cell_b)] -= volume_flux_m2_s
			/ grid_->cell(edge.cell_b).area_m2;
	}
	return divergence;
}

double VoronoiDryCore::rms_horizontal_divergence_s1(const State &state) const {
	long double weighted = 0.0L;
	long double area = 0.0L;
	for (int k = 0; k < LEVELS; ++k) {
		const auto divergence = horizontal_divergence_s1(state, k);
		for (int c = 0; c < grid_->cell_count(); ++c) {
			const long double a = static_cast<long double>(grid_->cell(c).area_m2);
			const long double d = static_cast<long double>(divergence[static_cast<size_t>(c)]);
			weighted += a * d * d;
			area += a;
		}
	}
	if (!(area > 0.0L)) {
		throw std::runtime_error("Dry-core divergence metric has invalid total area");
	}
	const double rms = std::sqrt(static_cast<double>(weighted / area));
	if (!std::isfinite(rms)) {
		throw std::runtime_error("Dry-core divergence metric became non-finite");
	}
	return rms;
}

VoronoiDryCore::DivergenceDampingDiagnostics
VoronoiDryCore::apply_divergence_damping(State &state, double dt_s) const {
	if (!(dt_s >= 0.0) || !std::isfinite(dt_s)) {
		throw std::invalid_argument("Divergence damping timestep must be finite and non-negative");
	}
	const size_t scalar_count = static_cast<size_t>(LEVELS)
		* static_cast<size_t>(grid_->cell_count());
	const size_t edge_count_total = static_cast<size_t>(LEVELS)
		* static_cast<size_t>(grid_->edge_count());
	if (state.layer_mass_kg_m2.size() != scalar_count
			|| state.theta_mass_kg_k_m2.size() != scalar_count
			|| state.edge_normal_mps.size() != edge_count_total) {
		throw std::invalid_argument("Divergence damping state arrays have wrong size");
	}

	DivergenceDampingDiagnostics d;
	d.requested_dt_s = dt_s;
	d.strength = divergence_damping_strength_;

	auto divergence_for_level = [&](const std::vector<double> &wind,
			std::vector<double> &divergence) {
		if (wind.size() != static_cast<size_t>(grid_->edge_count())) {
			throw std::runtime_error("Divergence damping level wind has wrong size");
		}
		divergence.assign(static_cast<size_t>(grid_->cell_count()), 0.0);
		for (int e = 0; e < grid_->edge_count(); ++e) {
			const auto &edge = grid_->edge(e);
			const double u = wind[static_cast<size_t>(e)];
			if (!std::isfinite(u)) {
				throw std::runtime_error("Divergence damping received non-finite wind");
			}
			const double flux = edge.edge_length_m * u;
			divergence[static_cast<size_t>(edge.cell_a)] += flux
				/ grid_->cell(edge.cell_a).area_m2;
			divergence[static_cast<size_t>(edge.cell_b)] -= flux
				/ grid_->cell(edge.cell_b).area_m2;
		}
	};

	long double before_sum = 0.0L;
	long double total_area = 0.0L;
	std::vector<double> level_wind(static_cast<size_t>(grid_->edge_count()));
	std::vector<double> divergence;
	for (int k = 0; k < LEVELS; ++k) {
		const size_t offset = static_cast<size_t>(k * grid_->edge_count());
		std::copy_n(state.edge_normal_mps.begin() + static_cast<std::ptrdiff_t>(offset),
			grid_->edge_count(), level_wind.begin());
		divergence_for_level(level_wind, divergence);
		for (int c = 0; c < grid_->cell_count(); ++c) {
			const double value = divergence[static_cast<size_t>(c)];
			d.max_abs_divergence_before_s1 = std::max(
				d.max_abs_divergence_before_s1, std::abs(value));
			const long double a = static_cast<long double>(grid_->cell(c).area_m2);
			before_sum += a * static_cast<long double>(value * value);
			total_area += a;
		}
	}
	if (!(total_area > 0.0L)) {
		throw std::runtime_error("Divergence damping has invalid total grid area");
	}
	d.rms_divergence_before_s1 = std::sqrt(static_cast<double>(before_sum / total_area));
	d.rms_divergence_after_s1 = d.rms_divergence_before_s1;
	d.max_abs_divergence_after_s1 = d.max_abs_divergence_before_s1;

	if (dt_s == 0.0 || divergence_damping_strength_ == 0.0
			|| d.max_abs_divergence_before_s1 <= QUIET_DIVERGENCE_S1) {
		d.applied_dt_s = dt_s;
		return d;
	}

	// The numerical viscosity uses potential temperature as a cheap local acoustic
	// scale. This intentionally avoids another full hydrostatic allocation in the
	// filter while retaining thermal and resolution scaling:
	//   nu_D = C_D * sqrt(gamma Rd theta_edge) * d_edge.
	// Actual fast-wave CFL is still enforced by VoronoiDryDynamics itself.
	d.max_diffusive_courant = 0.0;
	for (int k = 0; k < LEVELS; ++k) {
		for (int e = 0; e < grid_->edge_count(); ++e) {
			const auto &edge = grid_->edge(e);
			const size_t ia = static_cast<size_t>(k * grid_->cell_count() + edge.cell_a);
			const size_t ib = static_cast<size_t>(k * grid_->cell_count() + edge.cell_b);
			const double ma = state.layer_mass_kg_m2[ia];
			const double mb = state.layer_mass_kg_m2[ib];
			const double ta = state.theta_mass_kg_k_m2[ia] / ma;
			const double tb = state.theta_mass_kg_k_m2[ib] / mb;
			const double theta_edge = 0.5 * (ta + tb);
			if (!(ma > 0.0) || !(mb > 0.0) || !(theta_edge > 0.0)
					|| !std::isfinite(theta_edge)) {
				throw std::runtime_error("Divergence damping diagnosed invalid thermodynamic scale");
			}
			const double c_scale = std::sqrt(
				DIVERGENCE_GAMMA * VoronoiDryHydrostatic::RD * theta_edge);
			const double courant = divergence_damping_strength_ * c_scale * dt_s
				/ edge.center_distance_m;
			d.max_diffusive_courant = std::max(d.max_diffusive_courant, courant);
		}
	}
	if (!std::isfinite(d.max_diffusive_courant)) {
		throw std::runtime_error("Divergence damping CFL became non-finite");
	}
	d.substeps = std::max(1, static_cast<int>(std::ceil(
		d.max_diffusive_courant / MAX_DAMPING_SUBSTEP_COURANT)));
	if (d.substeps > MAX_DAMPING_SUBSTEPS) {
		throw std::runtime_error("Divergence damping requires excessive subcycling");
	}
	const double sub_dt = dt_s / static_cast<double>(d.substeps);

	std::vector<double> u0(static_cast<size_t>(grid_->edge_count()));
	std::vector<double> stage_a(static_cast<size_t>(grid_->edge_count()));
	std::vector<double> stage_b(static_cast<size_t>(grid_->edge_count()));
	std::vector<double> tendency(static_cast<size_t>(grid_->edge_count()));

	auto tendency_for_level = [&](int level, const std::vector<double> &wind,
			std::vector<double> &out) {
		divergence_for_level(wind, divergence);
		out.assign(static_cast<size_t>(grid_->edge_count()), 0.0);
		for (int e = 0; e < grid_->edge_count(); ++e) {
			const auto &edge = grid_->edge(e);
			const size_t ia = static_cast<size_t>(level * grid_->cell_count() + edge.cell_a);
			const size_t ib = static_cast<size_t>(level * grid_->cell_count() + edge.cell_b);
			const double ma = state.layer_mass_kg_m2[ia];
			const double mb = state.layer_mass_kg_m2[ib];
			const double theta_edge = 0.5 * (
				state.theta_mass_kg_k_m2[ia] / ma
				+ state.theta_mass_kg_k_m2[ib] / mb);
			const double c_scale = std::sqrt(
				DIVERGENCE_GAMMA * VoronoiDryHydrostatic::RD * theta_edge);
			// +nu grad(div u) is dissipative: for a Fourier divergent mode it is
			// -nu k^2 u_parallel. With nu=C*c*d, d cancels from this edge form.
			const double accel = divergence_damping_strength_ * c_scale * (
				divergence[static_cast<size_t>(edge.cell_b)]
				- divergence[static_cast<size_t>(edge.cell_a)]);
			if (!std::isfinite(accel)) {
				throw std::runtime_error("Divergence damping produced non-finite acceleration");
			}
			out[static_cast<size_t>(e)] = accel;
			d.max_damping_acceleration_mps2 = std::max(
				d.max_damping_acceleration_mps2, std::abs(accel));
		}
	};

	for (int k = 0; k < LEVELS; ++k) {
		const size_t offset = static_cast<size_t>(k * grid_->edge_count());
		for (int sub = 0; sub < d.substeps; ++sub) {
			std::copy_n(state.edge_normal_mps.begin() + static_cast<std::ptrdiff_t>(offset),
				grid_->edge_count(), u0.begin());

			// SSPRK3 stage 1.
			tendency_for_level(k, u0, tendency);
			for (int e = 0; e < grid_->edge_count(); ++e) {
				stage_a[static_cast<size_t>(e)] = u0[static_cast<size_t>(e)]
					+ sub_dt * tendency[static_cast<size_t>(e)];
			}

			// Euler stage from u1 followed by the 3/4-1/4 SSPRK blend.
			tendency_for_level(k, stage_a, tendency);
			for (int e = 0; e < grid_->edge_count(); ++e) {
				const size_t ie = static_cast<size_t>(e);
				stage_b[ie] = stage_a[ie] + sub_dt * tendency[ie];
				stage_a[ie] = 0.75 * u0[ie] + 0.25 * stage_b[ie];
			}

			// Final Euler stage and 1/3-2/3 SSPRK blend.
			tendency_for_level(k, stage_a, tendency);
			for (int e = 0; e < grid_->edge_count(); ++e) {
				const size_t ie = static_cast<size_t>(e);
				stage_b[ie] = stage_a[ie] + sub_dt * tendency[ie];
				const double filtered = (1.0 / 3.0) * u0[ie]
					+ (2.0 / 3.0) * stage_b[ie];
				if (!std::isfinite(filtered)) {
					throw std::runtime_error("Divergence damping produced non-finite wind");
				}
				state.edge_normal_mps[offset + ie] = filtered;
			}
		}
	}

	long double after_sum = 0.0L;
	for (int k = 0; k < LEVELS; ++k) {
		const size_t offset = static_cast<size_t>(k * grid_->edge_count());
		std::copy_n(state.edge_normal_mps.begin() + static_cast<std::ptrdiff_t>(offset),
			grid_->edge_count(), level_wind.begin());
		divergence_for_level(level_wind, divergence);
		for (int c = 0; c < grid_->cell_count(); ++c) {
			const double value = divergence[static_cast<size_t>(c)];
			d.max_abs_divergence_after_s1 = std::max(
				d.max_abs_divergence_after_s1, std::abs(value));
			const long double a = static_cast<long double>(grid_->cell(c).area_m2);
			after_sum += a * static_cast<long double>(value * value);
		}
	}
	d.rms_divergence_after_s1 = std::sqrt(static_cast<double>(after_sum / total_area));
	d.applied_dt_s = dt_s;
	d.applied = true;
	if (!std::isfinite(d.rms_divergence_after_s1)
			|| !std::isfinite(d.max_abs_divergence_after_s1)) {
		throw std::runtime_error("Divergence damping produced invalid diagnostics");
	}
	return d;
}

double VoronoiDryCore::max_coordinate_mass_fraction_error(const State &state) const {
	const size_t scalar_count = static_cast<size_t>(LEVELS) * grid_->cell_count();
	if (state.layer_mass_kg_m2.size() != scalar_count) {
		throw std::invalid_argument("Dry-core layer-mass array has wrong size");
	}
	const auto &reference = vertical_.reference_mass_fractions();
	double maximum = 0.0;
	for (int c = 0; c < grid_->cell_count(); ++c) {
		long double column_mass_ld = 0.0L;
		for (int k = 0; k < LEVELS; ++k) {
			const double mass = state.layer_mass_kg_m2[
				static_cast<size_t>(k * grid_->cell_count() + c)];
			if (!(mass > 0.0) || !std::isfinite(mass)) {
				throw std::runtime_error("Dry core has invalid layer mass while checking coordinate");
			}
			column_mass_ld += static_cast<long double>(mass);
		}
		const double column_mass = static_cast<double>(column_mass_ld);
		if (!(column_mass > 0.0) || !std::isfinite(column_mass)) {
			throw std::runtime_error("Dry core has invalid column mass while checking coordinate");
		}
		for (int k = 0; k < LEVELS; ++k) {
			const double actual = state.layer_mass_kg_m2[
				static_cast<size_t>(k * grid_->cell_count() + c)] / column_mass;
			maximum = std::max(maximum,
				std::abs(actual - reference[static_cast<size_t>(k)]));
		}
	}
	return maximum;
}

void VoronoiDryCore::refresh_extrema(const State &state,
		StepDiagnostics &diagnostics) const {
	diagnostics.min_layer_mass_kg_m2 = std::numeric_limits<double>::infinity();
	diagnostics.min_potential_temperature_k = std::numeric_limits<double>::infinity();
	diagnostics.max_speed_mps = 0.0;
	for (size_t i = 0; i < state.layer_mass_kg_m2.size(); ++i) {
		const double mass = state.layer_mass_kg_m2[i];
		const double theta_mass = state.theta_mass_kg_k_m2[i];
		if (!(mass > 0.0) || !(theta_mass > 0.0)
				|| !std::isfinite(mass) || !std::isfinite(theta_mass)) {
			throw std::runtime_error("Dry core extrema encountered invalid thermodynamic state");
		}
		diagnostics.min_layer_mass_kg_m2 = std::min(
			diagnostics.min_layer_mass_kg_m2, mass);
		diagnostics.min_potential_temperature_k = std::min(
			diagnostics.min_potential_temperature_k, theta_mass / mass);
	}
	for (double u : state.edge_normal_mps) {
		if (!std::isfinite(u)) throw std::runtime_error("Dry core extrema encountered invalid wind");
		diagnostics.max_speed_mps = std::max(diagnostics.max_speed_mps, std::abs(u));
	}
	for (const auto &tracer : state.tracer_mass_kg_m2) {
		for (double mass : tracer) {
			if (!(mass >= 0.0) || !std::isfinite(mass)) {
				throw std::runtime_error("Dry core extrema encountered invalid tracer mass");
			}
		}
	}
}

VoronoiDryCore::StepDiagnostics VoronoiDryCore::step(State &state,
		double requested_dt_s, double target_cfl, int max_retries) const {
	StepDiagnostics d;
	d.requested_dt_s = requested_dt_s;
	d.dry_mass_before_kg = total_dry_mass_kg(state);
	d.theta_mass_before_kg_k = total_theta_mass_kg_k(state);
	std::vector<double> tracer_before(state.tracer_mass_kg_m2.size(), 0.0);
	for (size_t tracer = 0; tracer < tracer_before.size(); ++tracer) {
		tracer_before[tracer] = total_tracer_mass_kg(state, static_cast<int>(tracer));
	}
	const State original = state;

	const auto horizontal = dynamics_.step(state, requested_dt_s,
		target_cfl, max_retries);
	d.accepted_dt_s = horizontal.accepted_dt_s;
	d.max_courant = horizontal.max_courant;
	d.max_pressure_acceleration_mps2 = horizontal.max_pressure_acceleration_mps2;
	d.max_relative_tracer_mass_error = horizontal.max_relative_tracer_mass_error;
	d.rejected_steps = horizontal.rejected_steps;

	if (!(horizontal.accepted_dt_s > 0.0)) {
		state = original;
		d.dry_mass_after_kg = d.dry_mass_before_kg;
		d.theta_mass_after_kg_k = d.theta_mass_before_kg_k;
		d.max_coordinate_mass_fraction_error = max_coordinate_mass_fraction_error(state);
		refresh_extrema(state, d);
		return d;
	}

	try {
		const double coordinate_error_before = max_coordinate_mass_fraction_error(state);
		if (coordinate_error_before > COORDINATE_ALREADY_ALIGNED_TOL) {
			const auto remap = vertical_.remap_to_reference_levels(state);
			d.coordinate_remap_applied = true;
			d.max_coordinate_mass_fraction_error = remap.max_mass_fraction_error;
			d.max_coordinate_column_mass_error = remap.max_column_mass_error;
			d.max_coordinate_column_theta_mass_error = remap.max_column_theta_mass_error;
			d.max_coordinate_column_tracer_mass_error = remap.max_column_tracer_mass_error;
			d.max_coordinate_edge_momentum_error = remap.max_edge_momentum_error;
			d.max_relative_tracer_mass_error = std::max(
				d.max_relative_tracer_mass_error, remap.max_relative_tracer_mass_error);
		} else {
			d.max_coordinate_mass_fraction_error = coordinate_error_before;
		}

		const auto damping = apply_divergence_damping(state, horizontal.accepted_dt_s);
		d.divergence_rms_before_s1 = damping.rms_divergence_before_s1;
		d.divergence_rms_after_s1 = damping.rms_divergence_after_s1;
		d.max_abs_divergence_before_s1 = damping.max_abs_divergence_before_s1;
		d.max_abs_divergence_after_s1 = damping.max_abs_divergence_after_s1;
		d.max_divergence_damping_acceleration_mps2 =
			damping.max_damping_acceleration_mps2;
		d.max_divergence_damping_courant = damping.max_diffusive_courant;
		d.divergence_damping_substeps = damping.substeps;
		d.divergence_damping_applied = damping.applied;
		if (damping.applied
				&& damping.rms_divergence_after_s1
					> damping.rms_divergence_before_s1 * (1.0 + ANTIDIFFUSIVE_REL_TOL)
					+ 1.0e-18) {
			throw std::runtime_error("Dry-core divergence damping became anti-diffusive");
		}

		d.dry_mass_after_kg = total_dry_mass_kg(state);
		d.theta_mass_after_kg_k = total_theta_mass_kg_k(state);
		d.relative_dry_mass_error = relative_error(
			d.dry_mass_after_kg, d.dry_mass_before_kg);
		d.relative_theta_mass_error = relative_error(
			d.theta_mass_after_kg_k, d.theta_mass_before_kg_k);
		for (size_t tracer = 0; tracer < tracer_before.size(); ++tracer) {
			d.max_relative_tracer_mass_error = std::max(
				d.max_relative_tracer_mass_error,
				relative_error(total_tracer_mass_kg(state, static_cast<int>(tracer)),
					tracer_before[tracer]));
		}

		if (!std::isfinite(d.relative_dry_mass_error)
				|| !std::isfinite(d.relative_theta_mass_error)
				|| !std::isfinite(d.max_relative_tracer_mass_error)
				|| d.relative_dry_mass_error > CORE_CONSERVATION_REJECT_TOL
				|| d.relative_theta_mass_error > CORE_CONSERVATION_REJECT_TOL
				|| d.max_relative_tracer_mass_error > CORE_CONSERVATION_REJECT_TOL) {
			throw std::runtime_error("Dry core combined step failed conservation gate");
		}
		refresh_extrema(state, d);
		return d;
	} catch (const std::exception &) {
		state = original;
		d.accepted_dt_s = 0.0;
		d.rejected_steps += 1;
		d.dry_mass_after_kg = d.dry_mass_before_kg;
		d.theta_mass_after_kg_k = d.theta_mass_before_kg_k;
		d.relative_dry_mass_error = 0.0;
		d.relative_theta_mass_error = 0.0;
		d.max_relative_tracer_mass_error = 0.0;
		d.coordinate_remap_applied = false;
		d.max_coordinate_column_mass_error = 0.0;
		d.max_coordinate_column_theta_mass_error = 0.0;
		d.max_coordinate_column_tracer_mass_error = 0.0;
		d.max_coordinate_edge_momentum_error = 0.0;
		d.divergence_rms_before_s1 = 0.0;
		d.divergence_rms_after_s1 = 0.0;
		d.max_abs_divergence_before_s1 = 0.0;
		d.max_abs_divergence_after_s1 = 0.0;
		d.max_divergence_damping_acceleration_mps2 = 0.0;
		d.max_divergence_damping_courant = 0.0;
		d.divergence_damping_substeps = 0;
		d.divergence_damping_applied = false;
		d.max_coordinate_mass_fraction_error = max_coordinate_mass_fraction_error(state);
		refresh_extrema(state, d);
		return d;
	}
}

} // namespace asterra::weather

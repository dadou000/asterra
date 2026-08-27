#include "voronoi_surface_exchange.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace asterra::weather {

namespace {

double relative_error(double before, double after, double scale) {
	return std::abs(after - before) / std::max(scale, 1.0);
}

} // namespace

VoronoiSurfaceExchange::VoronoiSurfaceExchange(const VoronoiDryTransport &transport,
		TracerIndices indices)
	: transport_(&transport), indices_(indices) {
	if (indices_.vapor < 0 || indices_.cloud_liquid < 0 || indices_.cloud_ice < 0) {
		throw std::invalid_argument("Surface exchange tracer indices must be non-negative");
	}
	if (indices_.vapor == indices_.cloud_liquid
			|| indices_.vapor == indices_.cloud_ice
			|| indices_.cloud_liquid == indices_.cloud_ice) {
		throw std::invalid_argument("Surface exchange water tracer indices must be distinct");
	}
}

VoronoiSurfaceExchange::SurfaceState VoronoiSurfaceExchange::make_uniform_surface_state(
		double water_kg_m2, double energy_j_m2) const {
	if (!(water_kg_m2 >= 0.0) || !std::isfinite(water_kg_m2)
			|| !std::isfinite(energy_j_m2)) {
		throw std::invalid_argument("Surface exchange initial reservoir is invalid");
	}
	SurfaceState surface;
	const size_t cells = static_cast<size_t>(transport_->grid().cell_count());
	surface.water_kg_m2.assign(cells, water_kg_m2);
	surface.energy_j_m2.assign(cells, energy_j_m2);
	return surface;
}

void VoronoiSurfaceExchange::validate_surface(const SurfaceState &surface) const {
	const size_t cells = static_cast<size_t>(transport_->grid().cell_count());
	if (surface.water_kg_m2.size() != cells || surface.energy_j_m2.size() != cells) {
		throw std::invalid_argument("Surface exchange reservoir arrays have wrong size");
	}
	for (double water : surface.water_kg_m2) {
		if (!(water >= 0.0) || !std::isfinite(water)) {
			throw std::runtime_error("Surface exchange encountered invalid surface water");
		}
	}
	for (double energy : surface.energy_j_m2) {
		if (!std::isfinite(energy)) {
			throw std::runtime_error("Surface exchange encountered invalid surface energy");
		}
	}
}

void VoronoiSurfaceExchange::validate_fluxes(
		const std::vector<double> &evaporation_kg_m2_s,
		const std::vector<double> &sensible_heat_w_m2, double dt_s) const {
	const size_t cells = static_cast<size_t>(transport_->grid().cell_count());
	if (evaporation_kg_m2_s.size() != cells || sensible_heat_w_m2.size() != cells) {
		throw std::invalid_argument("Surface exchange flux arrays have wrong size");
	}
	if (!(dt_s > 0.0) || !std::isfinite(dt_s)) {
		throw std::invalid_argument("Surface exchange timestep must be finite and positive");
	}
	for (double flux : evaporation_kg_m2_s) {
		if (!std::isfinite(flux)) {
			throw std::invalid_argument("Surface exchange evaporation flux is non-finite");
		}
	}
	for (double flux : sensible_heat_w_m2) {
		if (!std::isfinite(flux)) {
			throw std::invalid_argument("Surface exchange sensible-heat flux is non-finite");
		}
	}
}

double VoronoiSurfaceExchange::total_surface_water_kg(const SurfaceState &surface) const {
	validate_surface(surface);
	long double total = 0.0L;
	for (int c = 0; c < transport_->grid().cell_count(); ++c) {
		total += static_cast<long double>(surface.water_kg_m2[static_cast<size_t>(c)])
			* static_cast<long double>(transport_->grid().cell(c).area_m2);
	}
	return static_cast<double>(total);
}

double VoronoiSurfaceExchange::total_surface_energy_j(const SurfaceState &surface) const {
	validate_surface(surface);
	long double total = 0.0L;
	for (int c = 0; c < transport_->grid().cell_count(); ++c) {
		total += static_cast<long double>(surface.energy_j_m2[static_cast<size_t>(c)])
			* static_cast<long double>(transport_->grid().cell(c).area_m2);
	}
	return static_cast<double>(total);
}

double VoronoiSurfaceExchange::total_atmospheric_water_kg(const State &atmosphere) const {
	return transport_->total_tracer_mass_kg(atmosphere, indices_.vapor)
		+ transport_->total_tracer_mass_kg(atmosphere, indices_.cloud_liquid)
		+ transport_->total_tracer_mass_kg(atmosphere, indices_.cloud_ice);
}

double VoronoiSurfaceExchange::atmospheric_thermodynamic_energy_j(
		const State &atmosphere) const {
	const int highest = std::max({indices_.vapor, indices_.cloud_liquid, indices_.cloud_ice});
	if (highest < 0 || static_cast<int>(atmosphere.tracer_mass_kg_m2.size()) <= highest) {
		throw std::invalid_argument("Surface exchange requires configured water tracer slots");
	}
	const size_t expected = atmosphere.layer_mass_kg_m2.size();
	for (int tracer : {indices_.vapor, indices_.cloud_liquid, indices_.cloud_ice}) {
		if (atmosphere.tracer_mass_kg_m2[static_cast<size_t>(tracer)].size() != expected) {
			throw std::invalid_argument("Surface exchange water tracer field has wrong size");
		}
	}

	const auto hydro = transport_->diagnose_hydrostatic(atmosphere);
	const int cells = transport_->grid().cell_count();
	const auto &vapor = atmosphere.tracer_mass_kg_m2[static_cast<size_t>(indices_.vapor)];
	const auto &ice = atmosphere.tracer_mass_kg_m2[static_cast<size_t>(indices_.cloud_ice)];
	long double total = 0.0L;
	for (int k = 0; k < VoronoiDryTransport::LEVELS; ++k) {
		for (int c = 0; c < cells; ++c) {
			const size_t i = static_cast<size_t>(k * cells + c);
			const long double column_enthalpy =
				static_cast<long double>(VoronoiMoistThermodynamics::CP_DRY)
					* static_cast<long double>(hydro.temperature_k[i])
					* static_cast<long double>(atmosphere.layer_mass_kg_m2[i])
				+ static_cast<long double>(VoronoiMoistThermodynamics::LV0_J_KG)
					* static_cast<long double>(vapor[i])
				- static_cast<long double>(VoronoiMoistThermodynamics::LF0_J_KG)
					* static_cast<long double>(ice[i]);
			total += column_enthalpy
				* static_cast<long double>(transport_->grid().cell(c).area_m2);
		}
	}
	return static_cast<double>(total);
}

VoronoiSurfaceExchange::Diagnostics VoronoiSurfaceExchange::apply_fluxes(
		State &atmosphere, SurfaceState &surface,
		const std::vector<double> &evaporation_kg_m2_s,
		const std::vector<double> &sensible_heat_w_m2,
		double dt_s) const {
	const State atmosphere_before = atmosphere;
	const SurfaceState surface_before = surface;

	try {
		validate_surface(surface);
		validate_fluxes(evaporation_kg_m2_s, sensible_heat_w_m2, dt_s);

		const int highest = std::max({indices_.vapor, indices_.cloud_liquid, indices_.cloud_ice});
		if (highest < 0 || static_cast<int>(atmosphere.tracer_mass_kg_m2.size()) <= highest) {
			throw std::invalid_argument("Surface exchange requires configured water tracer slots");
		}
		const size_t scalar_count = atmosphere.layer_mass_kg_m2.size();
		for (int tracer : {indices_.vapor, indices_.cloud_liquid, indices_.cloud_ice}) {
			if (atmosphere.tracer_mass_kg_m2[static_cast<size_t>(tracer)].size() != scalar_count) {
				throw std::invalid_argument("Surface exchange water tracer field has wrong size");
			}
		}

		Diagnostics d;
		d.requested_dt_s = dt_s;
		d.atmosphere_water_before_kg = total_atmospheric_water_kg(atmosphere);
		d.surface_water_before_kg = total_surface_water_kg(surface);
		d.atmosphere_thermo_before_j = atmospheric_thermodynamic_energy_j(atmosphere);
		d.surface_energy_before_j = total_surface_energy_j(surface);

		const auto hydro = transport_->diagnose_hydrostatic(atmosphere);
		const int cells = transport_->grid().cell_count();
		auto &vapor = atmosphere.tracer_mass_kg_m2[static_cast<size_t>(indices_.vapor)];
		d.min_surface_water_kg_m2 = std::numeric_limits<double>::infinity();
		d.min_bottom_temperature_k = std::numeric_limits<double>::infinity();
		d.max_bottom_temperature_k = -std::numeric_limits<double>::infinity();

		for (int c = 0; c < cells; ++c) {
			const size_t i = static_cast<size_t>(c); // level 0 is the bottom layer
			const double dry_mass = atmosphere.layer_mass_kg_m2[i];
			if (!(dry_mass > 0.0) || !std::isfinite(dry_mass)) {
				throw std::runtime_error("Surface exchange encountered invalid bottom-layer dry mass");
			}

			const double dm = evaporation_kg_m2_s[static_cast<size_t>(c)] * dt_s;
			const double sensible_j_m2 = sensible_heat_w_m2[static_cast<size_t>(c)] * dt_s;
			const double latent_j_m2 = VoronoiMoistThermodynamics::LV0_J_KG * dm;

			const double new_surface_water = surface.water_kg_m2[static_cast<size_t>(c)] - dm;
			const double new_vapor = vapor[i] + dm;
			if (!(new_surface_water >= 0.0) || !std::isfinite(new_surface_water)) {
				throw std::runtime_error("Surface exchange evaporation exceeds available surface water");
			}
			if (!(new_vapor >= 0.0) || !std::isfinite(new_vapor)) {
				throw std::runtime_error("Surface exchange condensation exceeds available atmospheric vapor");
			}

			if (sensible_j_m2 != 0.0) {
				const double target_temperature = hydro.temperature_k[i]
					+ sensible_j_m2 / (VoronoiMoistThermodynamics::CP_DRY * dry_mass);
				if (!(target_temperature > 100.0) || !std::isfinite(target_temperature)) {
					throw std::runtime_error("Surface exchange sensible heat produced invalid bottom temperature");
				}
				const double pressure = hydro.layer_pressure_pa[i];
				const double exner = std::pow(
					pressure / VoronoiDryHydrostatic::P0_PA,
					VoronoiDryHydrostatic::KAPPA);
				const double target_theta = target_temperature / exner;
				if (!(target_theta > 100.0) || !std::isfinite(target_theta)) {
					throw std::runtime_error("Surface exchange sensible heat produced invalid potential temperature");
				}
				atmosphere.theta_mass_kg_k_m2[i] = dry_mass * target_theta;
			}

			surface.water_kg_m2[static_cast<size_t>(c)] = new_surface_water;
			surface.energy_j_m2[static_cast<size_t>(c)] -= sensible_j_m2 + latent_j_m2;
			if (!std::isfinite(surface.energy_j_m2[static_cast<size_t>(c)])) {
				throw std::runtime_error("Surface exchange produced non-finite surface energy");
			}
			vapor[i] = new_vapor;

			const long double area = static_cast<long double>(transport_->grid().cell(c).area_m2);
			if (dm >= 0.0) {
				d.evaporated_to_atmosphere_kg += static_cast<double>(
					static_cast<long double>(dm) * area);
			} else {
				d.condensed_to_surface_kg += static_cast<double>(
					static_cast<long double>(-dm) * area);
			}
			d.sensible_to_atmosphere_j += static_cast<double>(
				static_cast<long double>(sensible_j_m2) * area);
			d.latent_to_atmosphere_j += static_cast<double>(
				static_cast<long double>(latent_j_m2) * area);
		}

		const auto after_hydro = transport_->diagnose_hydrostatic(atmosphere);
		for (int c = 0; c < cells; ++c) {
			d.min_surface_water_kg_m2 = std::min(d.min_surface_water_kg_m2,
				surface.water_kg_m2[static_cast<size_t>(c)]);
			d.min_bottom_temperature_k = std::min(d.min_bottom_temperature_k,
				after_hydro.temperature_k[static_cast<size_t>(c)]);
			d.max_bottom_temperature_k = std::max(d.max_bottom_temperature_k,
				after_hydro.temperature_k[static_cast<size_t>(c)]);
		}

		d.atmosphere_water_after_kg = total_atmospheric_water_kg(atmosphere);
		d.surface_water_after_kg = total_surface_water_kg(surface);
		d.atmosphere_thermo_after_j = atmospheric_thermodynamic_energy_j(atmosphere);
		d.surface_energy_after_j = total_surface_energy_j(surface);

		const double water_before = d.atmosphere_water_before_kg + d.surface_water_before_kg;
		const double water_after = d.atmosphere_water_after_kg + d.surface_water_after_kg;
		const double water_scale = std::abs(d.atmosphere_water_before_kg)
			+ std::abs(d.surface_water_before_kg);
		d.relative_system_water_error = relative_error(water_before, water_after, water_scale);

		const double energy_before = d.atmosphere_thermo_before_j + d.surface_energy_before_j;
		const double energy_after = d.atmosphere_thermo_after_j + d.surface_energy_after_j;
		const double energy_scale = std::abs(d.atmosphere_thermo_before_j)
			+ std::abs(d.surface_energy_before_j);
		d.relative_system_energy_error = relative_error(energy_before, energy_after, energy_scale);

		if (!(d.relative_system_water_error < 5.0e-12)
				|| !(d.relative_system_energy_error < 5.0e-12)) {
			throw std::runtime_error("Surface exchange failed closed-system conservation gate");
		}
		return d;
	} catch (...) {
		atmosphere = atmosphere_before;
		surface = surface_before;
		throw;
	}
}

} // namespace asterra::weather

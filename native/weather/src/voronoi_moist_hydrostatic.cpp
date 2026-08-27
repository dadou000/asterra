#include "voronoi_moist_hydrostatic.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace asterra::weather {

VoronoiMoistHydrostatic::VoronoiMoistHydrostatic(
		const VoronoiDryTransport &transport, TracerIndices indices)
	: transport_(&transport), indices_(indices) {
	if (indices_.vapor < 0 || indices_.cloud_liquid < 0 || indices_.cloud_ice < 0) {
		throw std::invalid_argument("Moist hydrostatic tracer indices must be non-negative");
	}
	if (indices_.vapor == indices_.cloud_liquid
			|| indices_.vapor == indices_.cloud_ice
			|| indices_.cloud_liquid == indices_.cloud_ice) {
		throw std::invalid_argument("Moist hydrostatic water tracer indices must be distinct");
	}
}

void VoronoiMoistHydrostatic::validate_water_shape(const State &state) const {
	// Dry diagnosis performs the authoritative conservative-state shape,
	// positivity and finite-value validation, including every existing tracer.
	(void)transport_->diagnose_hydrostatic(state);
	const int highest = std::max({indices_.vapor, indices_.cloud_liquid, indices_.cloud_ice});
	if (highest < 0 || static_cast<int>(state.tracer_mass_kg_m2.size()) <= highest) {
		throw std::invalid_argument("Moist hydrostatic diagnosis requires configured water tracer slots");
	}
	const size_t expected = state.layer_mass_kg_m2.size();
	for (int tracer : {indices_.vapor, indices_.cloud_liquid, indices_.cloud_ice}) {
		if (state.tracer_mass_kg_m2[static_cast<size_t>(tracer)].size() != expected) {
			throw std::invalid_argument("Moist hydrostatic water tracer field has wrong size");
		}
	}
}

VoronoiMoistHydrostatic::Diagnostics VoronoiMoistHydrostatic::diagnose(
		const State &state) const {
	validate_water_shape(state);
	const int cells = transport_->grid().cell_count();
	const size_t scalar_count = static_cast<size_t>(LEVELS) * static_cast<size_t>(cells);

	Diagnostics d;
	d.surface_pressure_pa.resize(static_cast<size_t>(cells));
	d.interface_pressure_pa.resize(static_cast<size_t>(INTERFACES) * cells);
	d.layer_pressure_pa.resize(scalar_count);
	d.potential_temperature_k.resize(scalar_count);
	d.temperature_k.resize(scalar_count);
	d.virtual_temperature_k.resize(scalar_count);
	d.layer_total_mass_kg_m2.resize(scalar_count);
	d.interface_geopotential.resize(static_cast<size_t>(INTERFACES) * cells);
	d.layer_geopotential.resize(scalar_count);

	const auto &vapor = state.tracer_mass_kg_m2[static_cast<size_t>(indices_.vapor)];
	const auto &liquid = state.tracer_mass_kg_m2[static_cast<size_t>(indices_.cloud_liquid)];
	const auto &ice = state.tracer_mass_kg_m2[static_cast<size_t>(indices_.cloud_ice)];
	const double gravity = transport_->gravity_mps2();

	for (int c = 0; c < cells; ++c) {
		d.interface_pressure_pa[static_cast<size_t>(interface_index(LEVELS, c))]
			= transport_->top_pressure_pa();
		for (int k = LEVELS - 1; k >= 0; --k) {
			const size_t i = static_cast<size_t>(scalar_index(k, c));
			const double total_mass = state.layer_mass_kg_m2[i]
				+ vapor[i] + liquid[i] + ice[i];
			if (!(total_mass > 0.0) || !std::isfinite(total_mass)) {
				throw std::runtime_error("Moist hydrostatic layer total mass is invalid");
			}
			d.layer_total_mass_kg_m2[i] = total_mass;
			const double p_upper = d.interface_pressure_pa[
				static_cast<size_t>(interface_index(k + 1, c))];
			const double p_lower = p_upper + gravity * total_mass;
			if (!(p_lower > p_upper) || !std::isfinite(p_lower)) {
				throw std::runtime_error("Moist hydrostatic mass produced non-monotone pressure");
			}
			d.interface_pressure_pa[static_cast<size_t>(interface_index(k, c))] = p_lower;
		}
		d.surface_pressure_pa[static_cast<size_t>(c)]
			= d.interface_pressure_pa[static_cast<size_t>(interface_index(0, c))];

		d.interface_geopotential[static_cast<size_t>(interface_index(0, c))]
			= transport_->surface_geopotential_m2_s2()[static_cast<size_t>(c)];
		for (int k = 0; k < LEVELS; ++k) {
			const size_t i = static_cast<size_t>(scalar_index(k, c));
			const double dry_mass = state.layer_mass_kg_m2[i];
			const double theta = state.theta_mass_kg_k_m2[i] / dry_mass;
			const double p_lower = d.interface_pressure_pa[
				static_cast<size_t>(interface_index(k, c))];
			const double p_upper = d.interface_pressure_pa[
				static_cast<size_t>(interface_index(k + 1, c))];
			const double p_center = std::sqrt(p_lower * p_upper);
			const double temperature = theta * std::pow(
				p_center / VoronoiDryHydrostatic::P0_PA,
				VoronoiDryHydrostatic::KAPPA);
			const double qv = vapor[i] / dry_mass;
			const double ql = liquid[i] / dry_mass;
			const double qi = ice[i] / dry_mass;
			const double tv = VoronoiMoistThermodynamics::virtual_temperature_k(
				temperature, qv, ql, qi);
			const double phi_lower = d.interface_geopotential[
				static_cast<size_t>(interface_index(k, c))];
			const double log_full = std::log(p_lower / p_upper);
			const double log_half = std::log(p_lower / p_center);

			d.layer_pressure_pa[i] = p_center;
			d.potential_temperature_k[i] = theta;
			d.temperature_k[i] = temperature;
			d.virtual_temperature_k[i] = tv;
			d.layer_geopotential[i] = phi_lower
				+ VoronoiDryHydrostatic::RD * tv * log_half;
			d.interface_geopotential[
				static_cast<size_t>(interface_index(k + 1, c))]
				= phi_lower + VoronoiDryHydrostatic::RD * tv * log_full;
		}
	}
	return d;
}

std::vector<double> VoronoiMoistHydrostatic::pressure_gradient_acceleration(
		const State &state, const Diagnostics &d) const {
	validate_water_shape(state);
	const size_t cells = static_cast<size_t>(transport_->grid().cell_count());
	const size_t scalars = cells * LEVELS;
	if (d.surface_pressure_pa.size() != cells
			|| d.interface_pressure_pa.size() != cells * INTERFACES
			|| d.layer_pressure_pa.size() != scalars
			|| d.virtual_temperature_k.size() != scalars
			|| d.layer_geopotential.size() != scalars) {
		throw std::invalid_argument("Moist hydrostatic diagnostics have wrong size");
	}

	std::vector<double> acceleration(
		static_cast<size_t>(LEVELS) * transport_->grid().edge_count(), 0.0);
	for (int k = 0; k < LEVELS; ++k) {
		for (int e = 0; e < transport_->grid().edge_count(); ++e) {
			const auto &edge = transport_->grid().edge(e);
			const int ia = scalar_index(k, edge.cell_a);
			const int ib = scalar_index(k, edge.cell_b);
			const double inv_d = 1.0 / edge.center_distance_m;
			const double grad_phi = (d.layer_geopotential[static_cast<size_t>(ib)]
				- d.layer_geopotential[static_cast<size_t>(ia)]) * inv_d;
			const double grad_log_p = (std::log(d.layer_pressure_pa[static_cast<size_t>(ib)])
				- std::log(d.layer_pressure_pa[static_cast<size_t>(ia)])) * inv_d;
			const double tv_edge = 0.5 * (d.virtual_temperature_k[static_cast<size_t>(ia)]
				+ d.virtual_temperature_k[static_cast<size_t>(ib)]);
			acceleration[static_cast<size_t>(edge_index(k, e))]
				= -grad_phi - VoronoiDryHydrostatic::RD * tv_edge * grad_log_p;
		}
	}
	return acceleration;
}

double VoronoiMoistHydrostatic::total_moist_air_mass_kg(const State &state) const {
	validate_water_shape(state);
	return transport_->total_dry_mass_kg(state)
		+ transport_->total_tracer_mass_kg(state, indices_.vapor)
		+ transport_->total_tracer_mass_kg(state, indices_.cloud_liquid)
		+ transport_->total_tracer_mass_kg(state, indices_.cloud_ice);
}

} // namespace asterra::weather

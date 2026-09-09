#include "voronoi_moist_hydrostatic.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace asterra::weather {

VoronoiMoistHydrostatic::VoronoiMoistHydrostatic(
		const VoronoiDryTransport &transport, TracerIndices indices)
	: transport_(&transport), indices_(indices) {
	const std::array<int, 5> all{
		indices_.vapor, indices_.cloud_liquid, indices_.cloud_ice,
		indices_.rain, indices_.snow};
	for (int index : all) {
		if (index < 0) throw std::invalid_argument("Moist hydrostatic tracer indices must be non-negative");
	}
	for (size_t a = 0; a < all.size(); ++a) {
		for (size_t b = a + 1; b < all.size(); ++b) {
			if (all[a] == all[b]) {
				throw std::invalid_argument("Moist hydrostatic water tracer indices must be distinct");
			}
		}
	}
}

void VoronoiMoistHydrostatic::validate_water_shape(const State &state) const {
	const size_t scalar_count = static_cast<size_t>(LEVELS)
		* static_cast<size_t>(transport_->grid().cell_count());
	const size_t edge_count = static_cast<size_t>(LEVELS)
		* static_cast<size_t>(transport_->grid().edge_count());
	if (state.layer_mass_kg_m2.size() != scalar_count
			|| state.theta_mass_kg_k_m2.size() != scalar_count
			|| state.edge_normal_mps.size() != edge_count) {
		throw std::invalid_argument("Moist hydrostatic conservative-state arrays have wrong size");
	}
	for (size_t i = 0; i < scalar_count; ++i) {
		if (!(state.layer_mass_kg_m2[i] > 0.0)
				|| !(state.theta_mass_kg_k_m2[i] > 0.0)
				|| !std::isfinite(state.layer_mass_kg_m2[i])
				|| !std::isfinite(state.theta_mass_kg_k_m2[i])) {
			throw std::runtime_error("Moist hydrostatic diagnosis received invalid dry state");
		}
	}
	for (double u : state.edge_normal_mps) {
		if (!std::isfinite(u)) {
			throw std::runtime_error("Moist hydrostatic diagnosis received non-finite wind");
		}
	}
	for (const auto &tracer : state.tracer_mass_kg_m2) {
		if (tracer.size() != scalar_count) {
			throw std::invalid_argument("Moist hydrostatic tracer field has wrong size");
		}
		for (double mass : tracer) {
			if (!(mass >= 0.0) || !std::isfinite(mass)) {
				throw std::runtime_error("Moist hydrostatic diagnosis received invalid tracer mass");
			}
		}
	}
	const int highest = std::max({indices_.vapor, indices_.cloud_liquid,
		indices_.cloud_ice, indices_.rain, indices_.snow});
	if (highest < 0 || static_cast<int>(state.tracer_mass_kg_m2.size()) <= highest) {
		throw std::invalid_argument("Moist hydrostatic diagnosis requires configured water tracer slots");
	}
}

VoronoiMoistHydrostatic::Diagnostics VoronoiMoistHydrostatic::diagnose(
		const State &state) const {
	validate_water_shape(state);
	VoronoiMoistThermodynamics thermo(*transport_, indices_);
	const auto thermodynamic = thermo.diagnose_thermodynamics(state);
	const int cells = transport_->grid().cell_count();
	const size_t scalar_count = static_cast<size_t>(LEVELS) * static_cast<size_t>(cells);

	Diagnostics d;
	d.surface_pressure_pa = thermodynamic.surface_pressure_pa;
	d.interface_pressure_pa = thermodynamic.interface_pressure_pa;
	d.layer_pressure_pa = thermodynamic.layer_pressure_pa;
	d.potential_temperature_k = thermodynamic.potential_temperature_k;
	d.temperature_k = thermodynamic.temperature_k;
	d.virtual_temperature_k.resize(scalar_count);
	d.layer_total_mass_kg_m2.resize(scalar_count);
	d.interface_geopotential.resize(static_cast<size_t>(INTERFACES) * cells);
	d.layer_geopotential.resize(scalar_count);

	const auto &vapor = state.tracer_mass_kg_m2[static_cast<size_t>(indices_.vapor)];
	const auto &cloud_liquid = state.tracer_mass_kg_m2[static_cast<size_t>(indices_.cloud_liquid)];
	const auto &cloud_ice = state.tracer_mass_kg_m2[static_cast<size_t>(indices_.cloud_ice)];
	const auto &rain = state.tracer_mass_kg_m2[static_cast<size_t>(indices_.rain)];
	const auto &snow = state.tracer_mass_kg_m2[static_cast<size_t>(indices_.snow)];

	for (int c = 0; c < cells; ++c) {
		d.interface_geopotential[static_cast<size_t>(interface_index(0, c))]
			= transport_->surface_geopotential_m2_s2()[static_cast<size_t>(c)];
		for (int k = 0; k < LEVELS; ++k) {
			const size_t i = static_cast<size_t>(scalar_index(k, c));
			const double dry_mass = state.layer_mass_kg_m2[i];
			const double total_mass = dry_mass + vapor[i] + cloud_liquid[i]
				+ cloud_ice[i] + rain[i] + snow[i];
			const double p_lower = d.interface_pressure_pa[
				static_cast<size_t>(interface_index(k, c))];
			const double p_upper = d.interface_pressure_pa[
				static_cast<size_t>(interface_index(k + 1, c))];
			const double p_center = d.layer_pressure_pa[i];
			const double temperature = d.temperature_k[i];
			const double qv = vapor[i] / dry_mass;
			const double ql_total = (cloud_liquid[i] + rain[i]) / dry_mass;
			const double qi_total = (cloud_ice[i] + snow[i]) / dry_mass;
			const double tv = VoronoiMoistThermodynamics::virtual_temperature_k(
				temperature, qv, ql_total, qi_total);
			const double phi_lower = d.interface_geopotential[
				static_cast<size_t>(interface_index(k, c))];
			const double log_full = std::log(p_lower / p_upper);
			const double log_half = std::log(p_lower / p_center);

			d.layer_total_mass_kg_m2[i] = total_mass;
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
		+ transport_->total_tracer_mass_kg(state, indices_.cloud_ice)
		+ transport_->total_tracer_mass_kg(state, indices_.rain)
		+ transport_->total_tracer_mass_kg(state, indices_.snow);
}

} // namespace asterra::weather

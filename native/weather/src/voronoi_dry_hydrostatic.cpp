#include "voronoi_dry_hydrostatic.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace asterra::weather {

VoronoiDryHydrostatic::VoronoiDryHydrostatic(const GeodesicVoronoiGrid &grid,
		double gravity_mps2, double scale_height_m)
	: grid_(&grid), gravity_mps2_(gravity_mps2), scale_height_m_(scale_height_m) {
	if (grid.cell_count() <= 0 || grid.edge_count() <= 0) {
		throw std::invalid_argument("VoronoiDryHydrostatic requires a built geodesic grid");
	}
	if (!(gravity_mps2_ > 0.0) || !std::isfinite(gravity_mps2_)) {
		throw std::invalid_argument("VoronoiDryHydrostatic gravity must be finite and positive");
	}
	if (!(scale_height_m_ > 0.0) || !std::isfinite(scale_height_m_)) {
		throw std::invalid_argument("VoronoiDryHydrostatic scale height must be finite and positive");
	}

	interface_height_m_[0] = 0.0;
	for (int k = 1; k < LEVELS; ++k) {
		interface_height_m_[k] = 0.5 * (NOMINAL_HEIGHT_M[k - 1] + NOMINAL_HEIGHT_M[k]);
	}
	interface_height_m_[LEVELS] = NOMINAL_HEIGHT_M[LEVELS - 1]
		+ 0.5 * (NOMINAL_HEIGHT_M[LEVELS - 1] - NOMINAL_HEIGHT_M[LEVELS - 2]);

	for (int k = 0; k < INTERFACES; ++k) {
		sigma_interface_[k] = std::exp(-interface_height_m_[k] / scale_height_m_);
	}
	sigma_interface_[0] = 1.0;
	for (int k = 0; k < LEVELS; ++k) {
		if (!(sigma_interface_[k] > sigma_interface_[k + 1])) {
			throw std::runtime_error("Hydrostatic sigma interfaces are not strictly decreasing");
		}
	}
}

void VoronoiDryHydrostatic::validate(const State &state) const {
	const size_t cells = static_cast<size_t>(grid_->cell_count());
	const size_t edges = static_cast<size_t>(grid_->edge_count());
	if (state.surface_pressure_pa.size() != cells) {
		throw std::invalid_argument("Hydrostatic surface-pressure array has wrong size");
	}
	if (state.potential_temperature_k.size() != cells * LEVELS) {
		throw std::invalid_argument("Hydrostatic potential-temperature array has wrong size");
	}
	if (state.edge_normal_mps.size() != edges * LEVELS) {
		throw std::invalid_argument("Hydrostatic edge-wind array has wrong size");
	}
	for (double ps : state.surface_pressure_pa) {
		if (!(ps > 1000.0) || !std::isfinite(ps)) {
			throw std::runtime_error("Hydrostatic surface pressure is invalid");
		}
	}
	for (double theta : state.potential_temperature_k) {
		if (!(theta > 100.0) || !std::isfinite(theta)) {
			throw std::runtime_error("Hydrostatic potential temperature is invalid");
		}
	}
	for (double u : state.edge_normal_mps) {
		if (!std::isfinite(u)) throw std::runtime_error("Hydrostatic edge wind is invalid");
	}
}

VoronoiDryHydrostatic::State VoronoiDryHydrostatic::make_isothermal_reference(
		double surface_pressure_pa, double temperature_k) const {
	if (!(surface_pressure_pa > 1000.0) || !std::isfinite(surface_pressure_pa)) {
		throw std::invalid_argument("Reference surface pressure must be finite and positive");
	}
	if (!(temperature_k > 100.0) || !std::isfinite(temperature_k)) {
		throw std::invalid_argument("Reference temperature must be finite and positive");
	}
	State s;
	s.surface_pressure_pa.assign(static_cast<size_t>(grid_->cell_count()), surface_pressure_pa);
	s.potential_temperature_k.resize(static_cast<size_t>(grid_->cell_count()) * LEVELS);
	s.edge_normal_mps.assign(static_cast<size_t>(grid_->edge_count()) * LEVELS, 0.0);
	for (int k = 0; k < LEVELS; ++k) {
		const double p_lower = surface_pressure_pa * sigma_interface_[k];
		const double p_upper = surface_pressure_pa * sigma_interface_[k + 1];
		const double p_center = std::sqrt(p_lower * p_upper);
		const double theta = temperature_k * std::pow(P0_PA / p_center, KAPPA);
		for (int c = 0; c < grid_->cell_count(); ++c) {
			s.potential_temperature_k[static_cast<size_t>(scalar_index(k, c))] = theta;
		}
	}
	return s;
}

VoronoiDryHydrostatic::Diagnostics VoronoiDryHydrostatic::diagnose(const State &state) const {
	validate(state);
	const size_t cells = static_cast<size_t>(grid_->cell_count());
	Diagnostics d;
	d.interface_pressure_pa.resize(cells * INTERFACES);
	d.layer_pressure_pa.resize(cells * LEVELS);
	d.temperature_k.resize(cells * LEVELS);
	d.layer_mass_kg_m2.resize(cells * LEVELS);
	d.interface_geopotential.assign(cells * INTERFACES, 0.0);
	d.layer_geopotential.resize(cells * LEVELS);

	for (int c = 0; c < grid_->cell_count(); ++c) {
		const double ps = state.surface_pressure_pa[static_cast<size_t>(c)];
		for (int i = 0; i < INTERFACES; ++i) {
			d.interface_pressure_pa[static_cast<size_t>(interface_index(i, c))] = ps * sigma_interface_[i];
		}
		d.interface_geopotential[static_cast<size_t>(interface_index(0, c))] = 0.0;
		for (int k = 0; k < LEVELS; ++k) {
			const double p_lower = d.interface_pressure_pa[static_cast<size_t>(interface_index(k, c))];
			const double p_upper = d.interface_pressure_pa[static_cast<size_t>(interface_index(k + 1, c))];
			const double p_center = std::sqrt(p_lower * p_upper);
			const int si = scalar_index(k, c);
			const double theta = state.potential_temperature_k[static_cast<size_t>(si)];
			const double temperature = theta * std::pow(p_center / P0_PA, KAPPA);
			const double phi_lower = d.interface_geopotential[static_cast<size_t>(interface_index(k, c))];
			const double log_full = std::log(p_lower / p_upper);
			const double log_half = std::log(p_lower / p_center);
			d.layer_pressure_pa[static_cast<size_t>(si)] = p_center;
			d.temperature_k[static_cast<size_t>(si)] = temperature;
			d.layer_mass_kg_m2[static_cast<size_t>(si)] = (p_lower - p_upper) / gravity_mps2_;
			d.layer_geopotential[static_cast<size_t>(si)] = phi_lower + RD * temperature * log_half;
			d.interface_geopotential[static_cast<size_t>(interface_index(k + 1, c))]
				= phi_lower + RD * temperature * log_full;
		}
	}
	return d;
}

std::vector<double> VoronoiDryHydrostatic::pressure_gradient_acceleration(
		const State &state, const Diagnostics &diag) const {
	validate(state);
	const size_t expected_scalar = static_cast<size_t>(grid_->cell_count()) * LEVELS;
	if (diag.layer_pressure_pa.size() != expected_scalar
			|| diag.temperature_k.size() != expected_scalar
			|| diag.layer_geopotential.size() != expected_scalar) {
		throw std::invalid_argument("Hydrostatic diagnostics have wrong size");
	}
	std::vector<double> acceleration(static_cast<size_t>(grid_->edge_count()) * LEVELS, 0.0);
	for (int k = 0; k < LEVELS; ++k) {
		for (int e = 0; e < grid_->edge_count(); ++e) {
			const auto &edge = grid_->edge(e);
			const int ia = scalar_index(k, edge.cell_a);
			const int ib = scalar_index(k, edge.cell_b);
			const double inv_d = 1.0 / edge.center_distance_m;
			const double grad_phi = (diag.layer_geopotential[static_cast<size_t>(ib)]
				- diag.layer_geopotential[static_cast<size_t>(ia)]) * inv_d;
			const double grad_log_p = (std::log(diag.layer_pressure_pa[static_cast<size_t>(ib)])
				- std::log(diag.layer_pressure_pa[static_cast<size_t>(ia)])) * inv_d;
			const double t_edge = 0.5 * (diag.temperature_k[static_cast<size_t>(ia)]
				+ diag.temperature_k[static_cast<size_t>(ib)]);
			acceleration[static_cast<size_t>(edge_index(k, e))]
				= -grad_phi - RD * t_edge * grad_log_p;
		}
	}
	return acceleration;
}

double VoronoiDryHydrostatic::total_dry_air_mass_kg(const State &state) const {
	validate(state);
	long double total = 0.0L;
	for (int c = 0; c < grid_->cell_count(); ++c) {
		const long double column_mass = static_cast<long double>(state.surface_pressure_pa[static_cast<size_t>(c)])
			* (1.0L - static_cast<long double>(sigma_interface_[LEVELS]))
			/ static_cast<long double>(gravity_mps2_);
		total += column_mass * static_cast<long double>(grid_->cell(c).area_m2);
	}
	return static_cast<double>(total);
}

} // namespace asterra::weather

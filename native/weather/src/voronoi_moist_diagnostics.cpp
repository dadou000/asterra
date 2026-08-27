#include "voronoi_moist_thermodynamics.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace asterra::weather {

namespace {

bool finite_nonnegative_ratio(double q) {
	return std::isfinite(q) && q >= 0.0;
}

} // namespace

void VoronoiMoistThermodynamics::validate_water_state(const State &state) const {
	const size_t expected = static_cast<size_t>(scalar_count());
	if (state.layer_mass_kg_m2.size() != expected
			|| state.theta_mass_kg_k_m2.size() != expected) {
		throw std::invalid_argument("Moist thermodynamic state arrays have wrong size");
	}
	const int highest = std::max({indices_.vapor,
		indices_.cloud_liquid, indices_.cloud_ice});
	if (highest < 0 || static_cast<int>(state.tracer_mass_kg_m2.size()) <= highest) {
		throw std::invalid_argument("Moist diagnostics require configured water tracer slots");
	}
	for (const auto &tracer : state.tracer_mass_kg_m2) {
		if (tracer.size() != expected) {
			throw std::invalid_argument("Moist diagnostics encountered malformed tracer field");
		}
		for (double mass : tracer) {
			if (!(mass >= 0.0) || !std::isfinite(mass)) {
				throw std::runtime_error("Moist diagnostics received negative/non-finite tracer mass");
			}
	}
	}
	for (size_t i = 0; i < expected; ++i) {
		const double dry = state.layer_mass_kg_m2[i];
		const double theta_mass = state.theta_mass_kg_k_m2[i];
		if (!(dry > 0.0) || !(theta_mass > 0.0)
				|| !std::isfinite(dry) || !std::isfinite(theta_mass)) {
			throw std::runtime_error("Moist diagnostics received invalid dry thermodynamic state");
		}
	}
}

VoronoiMoistThermodynamics::ThermodynamicDiagnostics
VoronoiMoistThermodynamics::diagnose_thermodynamics(const State &state) const {
	validate_water_state(state);
	const int cells = transport_->grid().cell_count();
	const size_t scalar_count_value = static_cast<size_t>(LEVELS) * cells;
	ThermodynamicDiagnostics d;
	d.surface_pressure_pa.resize(static_cast<size_t>(cells));
	d.interface_pressure_pa.resize(static_cast<size_t>(INTERFACES) * cells);
	d.layer_pressure_pa.resize(scalar_count_value);
	d.potential_temperature_k.resize(scalar_count_value);
	d.temperature_k.resize(scalar_count_value);

	const auto &vapor = state.tracer_mass_kg_m2[static_cast<size_t>(indices_.vapor)];
	const auto &liquid = state.tracer_mass_kg_m2[static_cast<size_t>(indices_.cloud_liquid)];
	const auto &ice = state.tracer_mass_kg_m2[static_cast<size_t>(indices_.cloud_ice)];
	const double gravity = transport_->gravity_mps2();

	auto scalar_index = [cells](int level, int cell) {
		return static_cast<size_t>(level * cells + cell);
	};
	auto interface_index = [cells](int level, int cell) {
		return static_cast<size_t>(level * cells + cell);
	};

	for (int c = 0; c < cells; ++c) {
		d.interface_pressure_pa[interface_index(LEVELS, c)] = transport_->top_pressure_pa();
		for (int k = LEVELS - 1; k >= 0; --k) {
			const size_t i = scalar_index(k, c);
			const double total_mass = state.layer_mass_kg_m2[i]
				+ vapor[i] + liquid[i] + ice[i];
			if (!(total_mass > 0.0) || !std::isfinite(total_mass)) {
				throw std::runtime_error("Moist layer total mass is invalid");
			}
			const double p_upper = d.interface_pressure_pa[interface_index(k + 1, c)];
			const double p_lower = p_upper + gravity * total_mass;
			if (!(p_lower > p_upper) || !std::isfinite(p_lower)) {
				throw std::runtime_error("Moist layer mass produced non-monotone pressure");
			}
			d.interface_pressure_pa[interface_index(k, c)] = p_lower;
		}
		d.surface_pressure_pa[static_cast<size_t>(c)] = d.interface_pressure_pa[interface_index(0, c)];

		for (int k = 0; k < LEVELS; ++k) {
			const size_t i = scalar_index(k, c);
			const double p_lower = d.interface_pressure_pa[interface_index(k, c)];
			const double p_upper = d.interface_pressure_pa[interface_index(k + 1, c)];
			const double pressure = std::sqrt(p_lower * p_upper);
			const double theta = state.theta_mass_kg_k_m2[i] / state.layer_mass_kg_m2[i];
			const double temperature = theta * std::pow(
				pressure / VoronoiDryHydrostatic::P0_PA,
				VoronoiDryHydrostatic::KAPPA);
			if (!(pressure > 0.0) || !(temperature > 0.0)
					|| !std::isfinite(pressure) || !std::isfinite(temperature)) {
				throw std::runtime_error("Moist theta/full-pressure conversion produced invalid state");
			}
			d.layer_pressure_pa[i] = pressure;
			d.potential_temperature_k[i] = theta;
			d.temperature_k[i] = temperature;
		}
	}
	return d;
}

double VoronoiMoistThermodynamics::virtual_temperature_k(
		double temperature_k, double vapor_mixing_ratio,
		double liquid_mixing_ratio, double ice_mixing_ratio) {
	if (!(temperature_k > 0.0) || !std::isfinite(temperature_k)) {
		throw std::invalid_argument("Virtual temperature requires finite positive temperature");
	}
	if (!finite_nonnegative_ratio(vapor_mixing_ratio)
			|| !finite_nonnegative_ratio(liquid_mixing_ratio)
			|| !finite_nonnegative_ratio(ice_mixing_ratio)) {
		throw std::invalid_argument("Virtual temperature requires finite non-negative water mixing ratios");
	}
	const double total_water = vapor_mixing_ratio
		+ liquid_mixing_ratio + ice_mixing_ratio;
	const double numerator = 1.0 + vapor_mixing_ratio / EPSILON;
	const double denominator = 1.0 + total_water;
	const double result = temperature_k * numerator / denominator;
	if (!(result > 0.0) || !std::isfinite(result)) {
		throw std::runtime_error("Virtual temperature diagnostic produced invalid value");
	}
	return result;
}

double VoronoiMoistThermodynamics::mixture_density_kg_m3(
		double total_pressure_pa, double temperature_k,
		double vapor_mixing_ratio, double liquid_mixing_ratio,
		double ice_mixing_ratio) {
	if (!(total_pressure_pa > 0.0) || !std::isfinite(total_pressure_pa)) {
		throw std::invalid_argument("Moist mixture density requires finite positive total pressure");
	}
	const double tv = virtual_temperature_k(temperature_k,
		vapor_mixing_ratio, liquid_mixing_ratio, ice_mixing_ratio);
	const double density = total_pressure_pa / (VoronoiDryHydrostatic::RD * tv);
	if (!(density > 0.0) || !std::isfinite(density)) {
		throw std::runtime_error("Moist mixture density diagnostic produced invalid value");
	}
	return density;
}

std::vector<double> VoronoiMoistThermodynamics::diagnose_virtual_temperature_k(
		const State &state) const {
	const auto thermo = diagnose_thermodynamics(state);
	const size_t expected = state.layer_mass_kg_m2.size();
	std::vector<double> result(expected, 0.0);
	const auto &vapor = state.tracer_mass_kg_m2[static_cast<size_t>(indices_.vapor)];
	const auto &liquid = state.tracer_mass_kg_m2[static_cast<size_t>(indices_.cloud_liquid)];
	const auto &ice = state.tracer_mass_kg_m2[static_cast<size_t>(indices_.cloud_ice)];
	for (size_t i = 0; i < expected; ++i) {
		const double dry = state.layer_mass_kg_m2[i];
		result[i] = virtual_temperature_k(thermo.temperature_k[i],
			vapor[i] / dry, liquid[i] / dry, ice[i] / dry);
	}
	return result;
}

} // namespace asterra::weather

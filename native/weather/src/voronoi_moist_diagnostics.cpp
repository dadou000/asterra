#include "voronoi_moist_thermodynamics.h"

#include <cmath>
#include <stdexcept>

namespace asterra::weather {

namespace {

bool finite_nonnegative_ratio(double q) {
	return std::isfinite(q) && q >= 0.0;
}

} // namespace

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
	const int highest = std::max({indices_.vapor,
		indices_.cloud_liquid, indices_.cloud_ice});
	if (highest < 0 || static_cast<int>(state.tracer_mass_kg_m2.size()) <= highest) {
		throw std::invalid_argument("Virtual-temperature diagnostic requires configured water tracer slots");
	}
	const size_t expected = static_cast<size_t>(scalar_count());
	for (int tracer : {indices_.vapor, indices_.cloud_liquid, indices_.cloud_ice}) {
		if (state.tracer_mass_kg_m2[static_cast<size_t>(tracer)].size() != expected) {
			throw std::invalid_argument("Virtual-temperature water tracer field has wrong size");
		}
	}

	const auto hydro = transport_->diagnose_hydrostatic(state);
	std::vector<double> result(expected, 0.0);
	const auto &vapor = state.tracer_mass_kg_m2[static_cast<size_t>(indices_.vapor)];
	const auto &liquid = state.tracer_mass_kg_m2[static_cast<size_t>(indices_.cloud_liquid)];
	const auto &ice = state.tracer_mass_kg_m2[static_cast<size_t>(indices_.cloud_ice)];
	for (size_t i = 0; i < expected; ++i) {
		const double dry = state.layer_mass_kg_m2[i];
		if (!(dry > 0.0) || !std::isfinite(dry)) {
			throw std::runtime_error("Virtual-temperature diagnostic encountered invalid dry mass");
		}
		result[i] = virtual_temperature_k(hydro.temperature_k[i],
			vapor[i] / dry, liquid[i] / dry, ice[i] / dry);
	}
	return result;
}

} // namespace asterra::weather

#include "voronoi_moist_thermodynamics.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

using asterra::weather::GeodesicVoronoiGrid;
using asterra::weather::VoronoiDryHydrostatic;
using asterra::weather::VoronoiDryTransport;
using asterra::weather::VoronoiMoistThermodynamics;

namespace {
constexpr double R = 3500000.0;

void require(bool ok, const char *message) {
	if (!ok) throw std::runtime_error(message);
}

bool nearly_equal(double a, double b, double relative = 2e-13) {
	return std::abs(a - b) <= relative * std::max({std::abs(a), std::abs(b), 1.0});
}
} // namespace

int main() {
	try {
		constexpr double T = 300.0;
		constexpr double P = 100000.0;

		const double dry_tv = VoronoiMoistThermodynamics::virtual_temperature_k(T, 0.0, 0.0, 0.0);
		require(dry_tv == T, "zero-water virtual temperature is not exactly dry temperature");

		constexpr double qv = 0.020;
		const double vapor_tv = VoronoiMoistThermodynamics::virtual_temperature_k(T, qv, 0.0, 0.0);
		const double expected_vapor_tv = T * (1.0 + qv / VoronoiMoistThermodynamics::EPSILON)
			/ (1.0 + qv);
		require(nearly_equal(vapor_tv, expected_vapor_tv),
			"vapor virtual temperature does not match exact dry-basis ideal-mixture relation");
		require(vapor_tv > T,
			"water vapor should lower mixture density at fixed total pressure");

		constexpr double ql = 0.010;
		const double liquid_tv = VoronoiMoistThermodynamics::virtual_temperature_k(T, 0.0, ql, 0.0);
		require(nearly_equal(liquid_tv, T / (1.0 + ql)),
			"liquid loading virtual temperature is inconsistent");
		require(liquid_tv < T,
			"cloud liquid should increase mixture density at fixed pressure");

		constexpr double qi = 0.004;
		const double mixed_tv = VoronoiMoistThermodynamics::virtual_temperature_k(T, qv, ql, qi);
		const double expected_mixed_tv = T * (1.0 + qv / VoronoiMoistThermodynamics::EPSILON)
			/ (1.0 + qv + ql + qi);
		require(nearly_equal(mixed_tv, expected_mixed_tv),
			"mixed vapor/condensate virtual temperature is inconsistent");

		const double dry_density = VoronoiMoistThermodynamics::mixture_density_kg_m3(P, T, 0.0);
		const double vapor_density = VoronoiMoistThermodynamics::mixture_density_kg_m3(P, T, qv);
		const double liquid_density = VoronoiMoistThermodynamics::mixture_density_kg_m3(P, T, 0.0, ql);
		require(nearly_equal(dry_density, P / (VoronoiDryHydrostatic::RD * T)),
			"dry mixture-density limit is inconsistent with ideal gas law");
		require(vapor_density < dry_density,
			"vapor loading did not reduce density at fixed total pressure");
		require(liquid_density > dry_density,
			"liquid loading did not increase density at fixed total pressure");

		GeodesicVoronoiGrid grid(4, R);
		VoronoiDryTransport transport(grid, 9.80665, 8000.0, 7500.0);
		VoronoiMoistThermodynamics moist(transport);
		auto state = transport.make_isothermal_reference(110000.0, 288.0);
		moist.initialize_uniform_relative_humidity(state, 0.70);
		const auto hydro = transport.diagnose_hydrostatic(state);
		const auto virtual_temperature = moist.diagnose_virtual_temperature_k(state);
		require(virtual_temperature.size() == state.layer_mass_kg_m2.size(),
			"state virtual-temperature diagnostic returned wrong shape");

		double min_delta = 1e300;
		double max_delta = -1e300;
		for (size_t i = 0; i < virtual_temperature.size(); ++i) {
			const double delta = virtual_temperature[i] - hydro.temperature_k[i];
			require(std::isfinite(virtual_temperature[i]) && virtual_temperature[i] > 0.0,
				"state virtual-temperature diagnostic produced invalid value");
			min_delta = std::min(min_delta, delta);
			max_delta = std::max(max_delta, delta);
		}
		require(min_delta > 0.0,
			"uniform vapor field did not increase virtual temperature anywhere");

		// Adding condensate at fixed vapor/temperature must reduce Tv exactly by
		// its added inert mass in the denominator.
		for (size_t i = 0; i < state.layer_mass_kg_m2.size(); ++i) {
			state.tracer_mass_kg_m2[1][i] = 0.003 * state.layer_mass_kg_m2[i];
		}
		const auto loaded_tv = moist.diagnose_virtual_temperature_k(state);
		for (size_t i = 0; i < loaded_tv.size(); ++i) {
			require(loaded_tv[i] < virtual_temperature[i],
				"cloud-liquid loading failed to reduce virtual temperature");
		}

		std::cout << "VoronoiMoistDensity PASS\n"
			<< "  dry/vapor/liquid density: " << dry_density << "/"
			<< vapor_density << "/" << liquid_density << " kg/m3\n"
			<< "  70% RH Tv-T range: " << min_delta << ".." << max_delta << " K\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		std::cerr << "VoronoiMoistDensity FAIL: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}

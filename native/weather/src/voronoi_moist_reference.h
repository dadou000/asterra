#pragma once

#include "voronoi_dry_core.h"
#include "voronoi_moist_thermodynamics.h"

namespace asterra::weather {

// Construct zero-wind isothermal moist reference states over static terrain
// without violating the dry-mass vertical coordinate. Every column uses the
// core's exact reference dry-mass fractions. The column dry mass is solved so a
// fixed model-top pressure reaches one common top geopotential, while vapor is
// iterated to the requested relative humidity using full mechanical pressure.
class VoronoiMoistReference {
public:
	using State = VoronoiDryCore::State;
	using TracerIndices = VoronoiMoistThermodynamics::TracerIndices;

	struct Diagnostics {
		double max_relative_humidity_error = 0.0;
		double max_coordinate_mass_fraction_error = 0.0;
		double max_pressure_acceleration_mps2 = 0.0;
		double min_surface_pressure_pa = 0.0;
		double max_surface_pressure_pa = 0.0;
	};

	explicit VoronoiMoistReference(const VoronoiDryCore &core,
		TracerIndices indices = {});

	State make_isothermal_terrain_balanced(double reference_surface_pressure_pa,
		double temperature_k, double relative_humidity,
		Diagnostics *diagnostics = nullptr) const;

private:
	const VoronoiDryCore *core_ = nullptr;
	TracerIndices indices_{};
};

} // namespace asterra::weather

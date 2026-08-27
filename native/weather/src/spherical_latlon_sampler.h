#pragma once

#include "geodesic_voronoi_grid.h"

#include <vector>

namespace asterra::weather {

// Bilinear sampler for global equirectangular scalar rasters.
//
// Raster convention intentionally matches the weather presentation map:
//   - row-major [y * width + x]
//   - x increases west -> east and is periodic in longitude
//   - y increases north -> south
//   - samples are pixel-centred over longitude [-pi, pi) and latitude
//     [+pi/2, -pi/2]. The polar cap therefore uses the nearest top/bottom row.
//
// The physical weather core remains raster-independent; this helper exists at
// the runtime/input boundary so terrain, SST and future forcing maps can use the
// same seam-safe spherical resampling convention.
class SphericalLatLonSampler {
public:
	static double sample_bilinear(const std::vector<double> &raster,
		int width, int height, const Vec3d &direction);

	static std::vector<double> sample_to_voronoi_cells(
		const GeodesicVoronoiGrid &grid,
		const std::vector<double> &raster, int width, int height);

private:
	static void validate_raster(const std::vector<double> &raster,
		int width, int height);
};

} // namespace asterra::weather

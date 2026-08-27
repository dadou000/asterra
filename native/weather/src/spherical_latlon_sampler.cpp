#include "spherical_latlon_sampler.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace asterra::weather {

namespace {
constexpr double PI = 3.141592653589793238462643383279502884;

int wrap_x(int x, int width) {
	x %= width;
	if (x < 0) x += width;
	return x;
}

double sample_unchecked(const std::vector<double> &raster,
		int width, int height, const Vec3d &direction) {
	const double n2 = dot(direction, direction);
	if (!(n2 > 1.0e-24) || !std::isfinite(n2)) {
		throw std::invalid_argument("Spherical lat/lon sample direction must be finite and non-zero");
	}
	const Vec3d p = direction / std::sqrt(n2);
	const double lon = std::atan2(p.z, p.x);
	const double lat = std::asin(std::clamp(p.y, -1.0, 1.0));

	// Convert to the pixel-centre coordinate system used by the weather display.
	const double fx = (lon + PI) * static_cast<double>(width) / (2.0 * PI) - 0.5;
	const double fy = (0.5 * PI - lat) * static_cast<double>(height) / PI - 0.5;

	const double floor_x = std::floor(fx);
	const int x0_raw = static_cast<int>(floor_x);
	const int x1_raw = x0_raw + 1;
	const int x0 = wrap_x(x0_raw, width);
	const int x1 = wrap_x(x1_raw, width);
	const double tx = fx - floor_x;

	const double fy_clamped = std::clamp(fy, 0.0, static_cast<double>(height - 1));
	const int y0 = static_cast<int>(std::floor(fy_clamped));
	const int y1 = std::min(y0 + 1, height - 1);
	const double ty = fy_clamped - static_cast<double>(y0);

	auto at = [&](int x, int y) -> double {
		return raster[static_cast<size_t>(y) * static_cast<size_t>(width)
			+ static_cast<size_t>(x)];
	};
	const double north_west = at(x0, y0);
	const double north_east = at(x1, y0);
	const double south_west = at(x0, y1);
	const double south_east = at(x1, y1);
	const double north = north_west + tx * (north_east - north_west);
	const double south = south_west + tx * (south_east - south_west);
	return north + ty * (south - north);
}
} // namespace

void SphericalLatLonSampler::validate_raster(const std::vector<double> &raster,
		int width, int height) {
	if (width < 2 || height < 2) {
		throw std::invalid_argument("Spherical lat/lon raster dimensions must both be >= 2");
	}
	const size_t expected = static_cast<size_t>(width) * static_cast<size_t>(height);
	if (raster.size() != expected) {
		throw std::invalid_argument("Spherical lat/lon raster size does not match width*height");
	}
	for (double v : raster) {
		if (!std::isfinite(v)) {
			throw std::invalid_argument("Spherical lat/lon raster contains a non-finite sample");
		}
	}
}

double SphericalLatLonSampler::sample_bilinear(
		const std::vector<double> &raster, int width, int height,
		const Vec3d &direction) {
	validate_raster(raster, width, height);
	return sample_unchecked(raster, width, height, direction);
}

std::vector<double> SphericalLatLonSampler::sample_to_voronoi_cells(
		const GeodesicVoronoiGrid &grid,
		const std::vector<double> &raster, int width, int height) {
	validate_raster(raster, width, height);
	if (grid.cell_count() <= 0) {
		throw std::invalid_argument("Spherical lat/lon sampling requires a built Voronoi grid");
	}
	std::vector<double> out(static_cast<size_t>(grid.cell_count()));
	for (int c = 0; c < grid.cell_count(); ++c) {
		out[static_cast<size_t>(c)] = sample_unchecked(
			raster, width, height, grid.cell(c).center);
	}
	return out;
}

} // namespace asterra::weather

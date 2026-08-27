#include "spherical_latlon_sampler.h"
#include "voronoi_dry_core.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

using asterra::weather::GeodesicVoronoiGrid;
using asterra::weather::SphericalLatLonSampler;
using asterra::weather::Vec3d;
using asterra::weather::VoronoiDryCore;

namespace {
constexpr double PI = 3.141592653589793238462643383279502884;
constexpr double R = 3500000.0;
constexpr double G = 9.80665;
constexpr double P_REF = 110000.0;
constexpr double T = 288.0;

void require(bool ok, const char *message) {
	if (!ok) throw std::runtime_error(message);
}

double analytic(const Vec3d &p) {
	// Smooth periodic field expressed in Cartesian direction components. This
	// exercises both longitude seam handling and latitude interpolation.
	return 620.0 + 740.0 * p.y + 430.0 * p.x - 275.0 * p.z;
}

Vec3d direction_from_lat_lon(double lat, double lon) {
	const double clat = std::cos(lat);
	return {clat * std::cos(lon), std::sin(lat), clat * std::sin(lon)};
}
} // namespace

int main() {
	try {
		constexpr int W = 720;
		constexpr int H = 360;
		std::vector<double> raster(static_cast<size_t>(W) * H);
		for (int y = 0; y < H; ++y) {
			const double lat = 0.5 * PI - PI * (static_cast<double>(y) + 0.5) / H;
			for (int x = 0; x < W; ++x) {
				const double lon = -PI + 2.0 * PI * (static_cast<double>(x) + 0.5) / W;
				raster[static_cast<size_t>(y) * W + x]
					= analytic(direction_from_lat_lon(lat, lon));
			}
		}

		// Longitude wrapping must be continuous across the -pi/+pi seam.
		const double seam_lat = 0.31;
		const double eps = 1.0e-8;
		const double west = SphericalLatLonSampler::sample_bilinear(
			raster, W, H, direction_from_lat_lon(seam_lat, -PI + eps));
		const double east = SphericalLatLonSampler::sample_bilinear(
			raster, W, H, direction_from_lat_lon(seam_lat, PI - eps));
		require(std::abs(west - east) < 1.0e-3,
			"lat/lon sampler is discontinuous at the longitude seam");

		// Polar sampling must remain finite and use the nearest polar-cap row.
		const double north = SphericalLatLonSampler::sample_bilinear(
			raster, W, H, {0.0, 1.0, 0.0});
		const double south = SphericalLatLonSampler::sample_bilinear(
			raster, W, H, {0.0, -1.0, 0.0});
		require(std::isfinite(north) && std::isfinite(south),
			"lat/lon sampler produced non-finite polar values");
		require(std::abs(north - analytic({0.0, 1.0, 0.0})) < 5.0,
			"north-pole raster extrapolation is inaccurate");
		require(std::abs(south - analytic({0.0, -1.0, 0.0})) < 5.0,
			"south-pole raster extrapolation is inaccurate");

		GeodesicVoronoiGrid grid(8, R);
		const auto sampled = SphericalLatLonSampler::sample_to_voronoi_cells(
			grid, raster, W, H);
		require(sampled.size() == static_cast<size_t>(grid.cell_count()),
			"lat/lon sampler returned wrong Voronoi cell count");

		double max_error = 0.0;
		double rms = 0.0;
		for (int c = 0; c < grid.cell_count(); ++c) {
			const double expected = analytic(grid.cell(c).center);
			const double error = sampled[static_cast<size_t>(c)] - expected;
			max_error = std::max(max_error, std::abs(error));
			rms += error * error;
		}
		rms = std::sqrt(rms / static_cast<double>(grid.cell_count()));
		require(max_error < 0.08,
			"lat/lon -> Voronoi bilinear sampling exceeded max error bound");
		require(rms < 0.025,
			"lat/lon -> Voronoi bilinear sampling exceeded RMS error bound");

		// A constant terrain raster should remain bitwise constant after sampling.
		std::vector<double> constant(static_cast<size_t>(64 * 32), 1234.5);
		const auto constant_cells = SphericalLatLonSampler::sample_to_voronoi_cells(
			grid, constant, 64, 32);
		for (double v : constant_cells) {
			require(v == 1234.5, "constant spherical raster was not preserved exactly");
		}

		// End-to-end terrain boundary: the sampled raster becomes the static lower
		// boundary of the transactional dry core. A terrain-balanced isothermal
		// state must remain pressure-force free, proving that runtime resampling and
		// hydrostatic geometry use exactly the same cell ordering and sign convention.
		VoronoiDryCore core(grid, G, 8000.0, 7500.0, 0.0, {0.0, 1.0, 0.0});
		core.set_surface_height_m(sampled);
		const auto balanced = core.make_isothermal_terrain_balanced_reference(P_REF, T);
		const auto hydro = core.transport().diagnose_hydrostatic(balanced);
		const auto pressure = core.transport().pressure_gradient_acceleration(balanced, hydro);
		double max_balanced_accel = 0.0;
		for (double a : pressure) {
			max_balanced_accel = std::max(max_balanced_accel, std::abs(a));
		}
		require(max_balanced_accel < 5e-10,
			"raster-sampled terrain produced a spurious balanced pressure force");

		std::cout << "SphericalLatLonSampler PASS\n"
			<< "  Voronoi max/RMS error: " << max_error << "/" << rms << " m\n"
			<< "  seam delta: " << std::abs(west - east) << "\n"
			<< "  sampled-terrain balanced max accel: " << max_balanced_accel << " m/s2\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		std::cerr << "SphericalLatLonSampler FAIL: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}

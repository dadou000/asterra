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
	return 620.0 + 740.0 * p.y + 430.0 * p.x - 275.0 * p.z;
}

Vec3d direction_from_lat_lon(double lat, double lon) {
	const double clat = std::cos(lat);
	return {clat * std::cos(lon), std::sin(lat), clat * std::sin(lon)};
}

double row_mean(const std::vector<double> &raster, int width, int y) {
	long double sum = 0.0L;
	for (int x = 0; x < width; ++x) {
		sum += static_cast<long double>(raster[
			static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)]);
	}
	return static_cast<double>(sum / static_cast<long double>(width));
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

		const double seam_lat = 0.31;
		const double eps = 1.0e-8;
		const double west = SphericalLatLonSampler::sample_bilinear(
			raster, W, H, direction_from_lat_lon(seam_lat, -PI + eps));
		const double east = SphericalLatLonSampler::sample_bilinear(
			raster, W, H, direction_from_lat_lon(seam_lat, PI - eps));
		require(std::abs(west - east) < 1.0e-3,
			"lat/lon sampler is discontinuous at the longitude seam");

		const double north = SphericalLatLonSampler::sample_bilinear(
			raster, W, H, {0.0, 1.0, 0.0});
		const double south = SphericalLatLonSampler::sample_bilinear(
			raster, W, H, {0.0, -1.0, 0.0});
		const double north_row = row_mean(raster, W, 0);
		const double south_row = row_mean(raster, W, H - 1);
		require(std::isfinite(north) && std::isfinite(south),
			"lat/lon sampler produced non-finite polar values");
		require(north == north_row && south == south_row,
			"exact-pole raster sample depends on an arbitrary longitude");
		require(std::abs(north - analytic({0.0, 1.0, 0.0})) < 0.02,
			"north-pole row-mean extrapolation is inaccurate");
		require(std::abs(south - analytic({0.0, -1.0, 0.0})) < 0.02,
			"south-pole row-mean extrapolation is inaccurate");

		// Deliberately make the nearest polar rows strongly longitude-dependent.
		// A scalar field at the mathematical pole cannot depend on longitude, so
		// exact-pole sampling must return the row mean, independent of atan2(0,0)
		// or signed-zero behavior.
		constexpr int PW = 16;
		constexpr int PH = 8;
		std::vector<double> hostile_polar(static_cast<size_t>(PW) * PH, 7.0);
		for (int x = 0; x < PW; ++x) {
			hostile_polar[static_cast<size_t>(x)] = 1000.0 + 37.0 * x;
			hostile_polar[static_cast<size_t>(PH - 1) * PW + static_cast<size_t>(x)]
				= -500.0 + 23.0 * x;
		}
		const double hostile_north = SphericalLatLonSampler::sample_bilinear(
			hostile_polar, PW, PH, {-0.0, 1.0, 0.0});
		const double hostile_south = SphericalLatLonSampler::sample_bilinear(
			hostile_polar, PW, PH, {0.0, -1.0, -0.0});
		require(hostile_north == row_mean(hostile_polar, PW, 0),
			"north pole selected a longitude-dependent raster value");
		require(hostile_south == row_mean(hostile_polar, PW, PH - 1),
			"south pole selected a longitude-dependent raster value");

		GeodesicVoronoiGrid grid(8, R);
		const auto sampled = SphericalLatLonSampler::sample_to_voronoi_cells(
			grid, raster, W, H);
		require(sampled.size() == static_cast<size_t>(grid.cell_count()),
			"lat/lon sampler returned wrong Voronoi cell count");

		int north_cell = -1;
		int south_cell = -1;
		double north_y = -2.0;
		double south_y = 2.0;
		double max_error = 0.0;
		double rms = 0.0;
		for (int c = 0; c < grid.cell_count(); ++c) {
			const auto &center = grid.cell(c).center;
			if (center.y > north_y) { north_y = center.y; north_cell = c; }
			if (center.y < south_y) { south_y = center.y; south_cell = c; }
			const double expected = analytic(center);
			const double error = sampled[static_cast<size_t>(c)] - expected;
			max_error = std::max(max_error, std::abs(error));
			rms += error * error;
		}
		require(north_cell >= 0 && south_cell >= 0
				&& std::abs(north_y - 1.0) < 1.0e-14
				&& std::abs(south_y + 1.0) < 1.0e-14,
			"even-frequency geodesic regression did not contain exact pole cells");
		require(sampled[static_cast<size_t>(north_cell)] == north_row,
			"north geodesic pole cell did not use longitude-invariant raster sampling");
		require(sampled[static_cast<size_t>(south_cell)] == south_row,
			"south geodesic pole cell did not use longitude-invariant raster sampling");

		rms = std::sqrt(rms / static_cast<double>(grid.cell_count()));
		std::cout << "SphericalLatLonSampler interpolation diagnostics\n"
			<< "  Voronoi max/RMS error: " << max_error << "/" << rms << " m\n";

		// At 720x360, the pixel centres stop half a pixel (0.25 degrees) short of
		// each pole. The sampler clamps the unresolved polar half-caps to the
		// nearest row and collapses the exact mathematical pole to that row's mean.
		// The remaining worst-case gate is therefore tied to pixel resolution.
		require(max_error < 5.0,
			"lat/lon -> Voronoi sampling exceeded pixel-resolution max error bound");
		require(rms < 0.25,
			"lat/lon -> Voronoi sampling exceeded pixel-resolution RMS error bound");

		std::vector<double> constant(static_cast<size_t>(64 * 32), 1234.5);
		const auto constant_cells = SphericalLatLonSampler::sample_to_voronoi_cells(
			grid, constant, 64, 32);
		for (double v : constant_cells) {
			require(v == 1234.5, "constant spherical raster was not preserved exactly");
		}

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
			<< "  seam delta: " << std::abs(west - east) << "\n"
			<< "  exact pole cell ids: " << north_cell << "/" << south_cell << "\n"
			<< "  sampled-terrain balanced max accel: " << max_balanced_accel << " m/s2\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		std::cerr << "SphericalLatLonSampler FAIL: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}

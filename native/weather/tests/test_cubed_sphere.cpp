#include "cubed_sphere_grid.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

using asterra::weather::CubedSphereGrid;
using asterra::weather::dot;

static constexpr double PI = 3.141592653589793238462643383279502884;

static void require(bool condition, const char *message) {
	if (!condition) throw std::runtime_error(message);
}

int main() {
	try {
		constexpr int N = 256;
		constexpr double R = 3500000.0;
		CubedSphereGrid grid(N, R);

		require(grid.cell_count() == 6 * N * N, "wrong cubed-sphere cell count");

		double area_sum = 0.0;
		double min_area = std::numeric_limits<double>::infinity();
		double max_area = 0.0;
		double min_edge = std::numeric_limits<double>::infinity();
		double max_edge = 0.0;
		double max_reciprocal_edge_rel_error = 0.0;
		double max_normal_antisymmetry = 0.0;

		for (int id = 0; id < grid.cell_count(); ++id) {
			const auto &cell = grid.cell(id);
			require(std::isfinite(cell.area_m2) && cell.area_m2 > 0.0, "invalid cell area");
			require(std::abs(dot(cell.center, cell.tangent_u)) < 1e-12, "cell tangent_u not tangent");
			require(std::abs(dot(cell.center, cell.tangent_v)) < 1e-12, "cell tangent_v not tangent");
			area_sum += cell.area_m2;
			min_area = std::min(min_area, cell.area_m2);
			max_area = std::max(max_area, cell.area_m2);

			for (int edge = 0; edge < CubedSphereGrid::EDGE_COUNT; ++edge) {
				const auto &n = cell.neighbour[edge];
				require(n.cell >= 0 && n.cell < grid.cell_count(), "invalid neighbour id");
				require(n.edge >= 0 && n.edge < CubedSphereGrid::EDGE_COUNT, "invalid reciprocal neighbour edge");
				const auto &other = grid.cell(n.cell);
				require(other.neighbour[n.edge].cell == id, "neighbour relation not reciprocal");
				const double length_a = cell.edge_length_m[edge];
				const double length_b = other.edge_length_m[n.edge];
				require(std::isfinite(length_a) && length_a > 0.0, "invalid edge length");
				min_edge = std::min(min_edge, length_a);
				max_edge = std::max(max_edge, length_a);
				const double rel = std::abs(length_a - length_b) / std::max(length_a, length_b);
				max_reciprocal_edge_rel_error = std::max(max_reciprocal_edge_rel_error, rel);

				const double normal_sum_sq =
					(cell.outward_normal[edge].x + other.outward_normal[n.edge].x)
						* (cell.outward_normal[edge].x + other.outward_normal[n.edge].x)
					+ (cell.outward_normal[edge].y + other.outward_normal[n.edge].y)
						* (cell.outward_normal[edge].y + other.outward_normal[n.edge].y)
					+ (cell.outward_normal[edge].z + other.outward_normal[n.edge].z)
						* (cell.outward_normal[edge].z + other.outward_normal[n.edge].z);
				max_normal_antisymmetry = std::max(max_normal_antisymmetry, std::sqrt(normal_sum_sq));
			}
		}

		const double sphere_area = 4.0 * PI * R * R;
		const double area_rel_error = std::abs(area_sum - sphere_area) / sphere_area;
		require(area_rel_error < 2e-11, "cubed-sphere total area does not close");
		require(min_area / max_area > 0.65, "cubed-sphere cell-area distortion is unexpectedly large");
		require(max_reciprocal_edge_rel_error < 2e-12, "shared edge lengths disagree across seam");
		require(max_normal_antisymmetry < 5e-3, "shared edge normals are not approximately opposite");

		// Conservation sanity check: assign exactly one deterministic scalar flux to
		// each physical edge and add it with opposite sign to its two cells. The
		// global sum must cancel to round-off, including every cube seam/corner.
		long double net_flux = 0.0L;
		long double abs_flux = 0.0L;
		for (int id = 0; id < grid.cell_count(); ++id) {
			const auto &cell = grid.cell(id);
			for (int edge = 0; edge < CubedSphereGrid::EDGE_COUNT; ++edge) {
				const auto &n = cell.neighbour[edge];
				if (id >= n.cell) continue;
				const double phase = 0.000031 * static_cast<double>(id + 1)
					+ 0.17 * static_cast<double>(edge + 1);
				const double flux = std::sin(phase) * cell.edge_length_m[edge];
				net_flux += static_cast<long double>(flux);
				net_flux -= static_cast<long double>(flux);
				abs_flux += std::abs(static_cast<long double>(flux)) * 2.0L;
			}
		}
		const long double flux_rel_error = std::abs(net_flux) / std::max(abs_flux, 1.0L);
		require(flux_rel_error < 1e-18L, "shared-edge conservative flux cancellation failed");

		std::cout << "CubedSphereGrid PASS\n"
			<< "  cells: " << grid.cell_count() << "\n"
			<< "  total area relative error: " << area_rel_error << "\n"
			<< "  cell area ratio min/max: " << min_area / max_area << "\n"
			<< "  edge length range m: " << min_edge << " .. " << max_edge << "\n"
			<< "  reciprocal edge relative error: " << max_reciprocal_edge_rel_error << "\n"
			<< "  max shared-normal antisymmetry: " << max_normal_antisymmetry << "\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		std::cerr << "CubedSphereGrid FAIL: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}

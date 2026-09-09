#include "geodesic_voronoi_grid.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

using asterra::weather::GeodesicVoronoiGrid;
using asterra::weather::dot;

static constexpr double PI = 3.141592653589793238462643383279502884;

static void require(bool condition, const char *message) {
	if (!condition) throw std::runtime_error(message);
}

int main() {
	try {
		constexpr int F = 16;
		constexpr double R = 3500000.0;
		GeodesicVoronoiGrid grid(F, R);
		require(grid.cell_count() == GeodesicVoronoiGrid::expected_cell_count(F), "wrong geodesic cell count");
		require(grid.edge_count() == GeodesicVoronoiGrid::expected_edge_count(F), "wrong geodesic edge count");
		require(grid.vertex_count() == GeodesicVoronoiGrid::expected_vertex_count(F), "wrong geodesic vertex count");
		require(grid.cell_count() - grid.edge_count() + grid.vertex_count() == 2, "geodesic Euler characteristic is not two");

		long double cell_area_sum = 0.0L;
		long double dual_area_sum = 0.0L;
		double min_cell_area = std::numeric_limits<double>::infinity();
		double max_cell_area = 0.0;
		double min_edge_length = std::numeric_limits<double>::infinity();
		double max_edge_length = 0.0;
		double max_orthogonality_error = 0.0;
		int pentagons = 0;
		int hexagons = 0;
		int north_pole_cell = -1;
		int south_pole_cell = -1;
		double max_y = -2.0;
		double min_y = 2.0;

		for (int c = 0; c < grid.cell_count(); ++c) {
			const auto &cell = grid.cell(c);
			require(std::isfinite(cell.area_m2) && cell.area_m2 > 0.0, "invalid Voronoi cell area");
			require(cell.edges.size() == cell.neighbours.size(), "cell edge/neighbour degree mismatch");
			require(cell.edges.size() == cell.vertices.size(), "cell edge/vertex degree mismatch");
			if (cell.edges.size() == 5) ++pentagons;
			else if (cell.edges.size() == 6) ++hexagons;
			else throw std::runtime_error("Voronoi cell is neither pentagon nor hexagon");
			if (cell.center.y > max_y) { max_y = cell.center.y; north_pole_cell = c; }
			if (cell.center.y < min_y) { min_y = cell.center.y; south_pole_cell = c; }
			cell_area_sum += cell.area_m2;
			min_cell_area = std::min(min_cell_area, cell.area_m2);
			max_cell_area = std::max(max_cell_area, cell.area_m2);
		}
		require(pentagons == 12, "icosahedral Voronoi mesh must have exactly 12 pentagons");
		require(hexagons == grid.cell_count() - 12, "all non-pentagon cells must be hexagons");

		// Even subdivision frequencies used by the runtime contain exact +/-Y
		// pole cells. They must be ordinary finite Voronoi cells, not coordinate
		// singularities or special topological caps.
		require(north_pole_cell >= 0 && south_pole_cell >= 0,
			"geodesic mesh did not identify polar cells");
		const auto &north_pole = grid.cell(north_pole_cell);
		const auto &south_pole = grid.cell(south_pole_cell);
		require(std::abs(north_pole.center.x) < 1.0e-14
				&& std::abs(north_pole.center.y - 1.0) < 1.0e-14
				&& std::abs(north_pole.center.z) < 1.0e-14,
			"north geodesic pole is not an exact finite +Y cell");
		require(std::abs(south_pole.center.x) < 1.0e-14
				&& std::abs(south_pole.center.y + 1.0) < 1.0e-14
				&& std::abs(south_pole.center.z) < 1.0e-14,
			"south geodesic pole is not an exact finite -Y cell");
		require(north_pole.edges.size() == 6 && south_pole.edges.size() == 6,
			"polar cells are not regular six-neighbour Voronoi cells");

		for (int v = 0; v < grid.vertex_count(); ++v) {
			const auto &vertex = grid.vertex(v);
			require(std::isfinite(vertex.dual_area_m2) && vertex.dual_area_m2 > 0.0, "invalid Delaunay triangle area");
			dual_area_sum += vertex.dual_area_m2;
			for (int q = 0; q < 3; ++q) {
				require(vertex.cells[q] >= 0 && vertex.cells[q] < grid.cell_count(), "invalid dual-vertex cell id");
				require(vertex.edges[q] >= 0 && vertex.edges[q] < grid.edge_count(), "invalid dual-vertex edge id");
			}
		}

		for (int e = 0; e < grid.edge_count(); ++e) {
			const auto &edge = grid.edge(e);
			require(edge.cell_a >= 0 && edge.cell_a < grid.cell_count(), "invalid edge cell_a");
			require(edge.cell_b >= 0 && edge.cell_b < grid.cell_count(), "invalid edge cell_b");
			require(edge.vertex_a >= 0 && edge.vertex_a < grid.vertex_count(), "invalid edge vertex_a");
			require(edge.vertex_b >= 0 && edge.vertex_b < grid.vertex_count(), "invalid edge vertex_b");
			require(edge.center_distance_m > 0.0 && edge.edge_length_m > 0.0 && edge.edge_area_m2 > 0.0,
				"invalid primal/dual edge metric");
			min_edge_length = std::min(min_edge_length, edge.edge_length_m);
			max_edge_length = std::max(max_edge_length, edge.edge_length_m);
			max_orthogonality_error = std::max(max_orthogonality_error,
				std::abs(dot(edge.normal_a_to_b, edge.tangent_a_to_b)));
		}

		const long double sphere_area = 4.0L * static_cast<long double>(PI) * R * R;
		const double cell_area_error = static_cast<double>(std::abs(cell_area_sum - sphere_area) / sphere_area);
		const double dual_area_error = static_cast<double>(std::abs(dual_area_sum - sphere_area) / sphere_area);
		require(cell_area_error < 2e-10, "Voronoi cell areas do not close to sphere area");
		require(dual_area_error < 2e-12, "Delaunay triangle areas do not close to sphere area");
		require(min_cell_area / max_cell_area > 0.50, "geodesic Voronoi area distortion is excessive");
		require(max_orthogonality_error < 1e-8, "Voronoi edge is not orthogonal to cell-center connection");

		constexpr int PRODUCTION_F = 196;
		const double production_area_scale_km = std::sqrt(
			4.0 * PI * R * R / static_cast<double>(GeodesicVoronoiGrid::expected_cell_count(PRODUCTION_F))) / 1000.0;
		require(production_area_scale_km > 19.5 && production_area_scale_km < 20.5,
			"production geodesic frequency is not approximately 20 km");

		std::cout << "GeodesicVoronoiGrid PASS\n"
			<< "  frequency: " << F << "\n"
			<< "  cells/edges/vertices: " << grid.cell_count() << "/" << grid.edge_count() << "/" << grid.vertex_count() << "\n"
			<< "  pentagons/hexagons: " << pentagons << "/" << hexagons << "\n"
			<< "  exact north/south pole cells: " << north_pole_cell << "/" << south_pole_cell << "\n"
			<< "  cell area relative closure: " << cell_area_error << "\n"
			<< "  dual area relative closure: " << dual_area_error << "\n"
			<< "  cell area min/max ratio: " << min_cell_area / max_cell_area << "\n"
			<< "  Voronoi edge length range m: " << min_edge_length << " .. " << max_edge_length << "\n"
			<< "  max primal/dual orthogonality error: " << max_orthogonality_error << "\n"
			<< "  L0 frequency " << PRODUCTION_F << " cells: "
			<< GeodesicVoronoiGrid::expected_cell_count(PRODUCTION_F) << "\n"
			<< "  L0 sqrt(area) scale km: " << production_area_scale_km << "\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		std::cerr << "GeodesicVoronoiGrid FAIL: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}

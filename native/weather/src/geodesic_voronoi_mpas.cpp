#include "geodesic_voronoi_grid.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace asterra::weather {

namespace {

bool edge_has_vertices(const GeodesicVoronoiGrid::EdgeGeometry &edge, int a, int b) {
	return (edge.vertex_a == a && edge.vertex_b == b)
		|| (edge.vertex_a == b && edge.vertex_b == a);
}

int shared_vertex(const GeodesicVoronoiGrid::EdgeGeometry &a,
		const GeodesicVoronoiGrid::EdgeGeometry &b) {
	if (a.vertex_a == b.vertex_a || a.vertex_a == b.vertex_b) return a.vertex_a;
	if (a.vertex_b == b.vertex_a || a.vertex_b == b.vertex_b) return a.vertex_b;
	return -1;
}

} // namespace

void GeodesicVoronoiGrid::finalize_mpas_metrics() {
	if (cells_.empty() || edges_.empty() || vertices_.empty()) {
		throw std::runtime_error("Cannot finalize MPAS metrics on an empty geodesic grid");
	}

	// MPAS positive edge normal points from cellsOnEdge(1) to cellsOnEdge(2).
	// Positive tangent is k x n. Orient the dual endpoint pair consistently with
	// that tangent so (normal,tangent,radial) is right handed everywhere.
	for (EdgeGeometry &edge : edges_) {
		const Vec3d positive_tangent = normalized(cross(edge.midpoint, edge.normal_a_to_b));
		Vec3d endpoint_direction = project_tangent(
			vertices_[static_cast<size_t>(edge.vertex_b)].center
				- vertices_[static_cast<size_t>(edge.vertex_a)].center,
			edge.midpoint);
		if (dot(endpoint_direction, positive_tangent) < 0.0) {
			std::swap(edge.vertex_a, edge.vertex_b);
		}
		edge.tangent_a_to_b = positive_tangent;
		edge.reconstruction_edges.clear();
		edge.reconstruction_weights.clear();
	}
	for (VertexGeometry &vertex : vertices_) vertex.kite_area_m2.fill(0.0);

	// Reorder each cell exactly as required by the MPAS mesh specification.
	// Vertices are already CCW. edges[q] is the boundary segment from
	// vertices[q-1] to vertices[q], so vertices[q] lies CCW of edges[q].
	for (int c = 0; c < cell_count(); ++c) {
		CellGeometry &cell = cells_[static_cast<size_t>(c)];
		const std::vector<int> candidate_edges = cell.edges;
		const size_t degree = cell.vertices.size();
		if (degree != 5 && degree != 6) {
			throw std::runtime_error("MPAS finalization encountered non pentagon/hexagon cell");
		}
		std::vector<int> ordered_edges(degree, -1);
		std::vector<int> ordered_neighbours(degree, -1);
		for (size_t q = 0; q < degree; ++q) {
			const int v_prev = cell.vertices[(q + degree - 1) % degree];
			const int v_cur = cell.vertices[q];
			for (int e : candidate_edges) {
				if (!edge_has_vertices(edges_[static_cast<size_t>(e)], v_prev, v_cur)) continue;
				if (ordered_edges[q] >= 0) {
					throw std::runtime_error("Multiple Voronoi edges match one ordered cell segment");
				}
				ordered_edges[q] = e;
				const EdgeGeometry &edge = edges_[static_cast<size_t>(e)];
				ordered_neighbours[q] = edge.cell_a == c ? edge.cell_b : edge.cell_a;
			}
			if (ordered_edges[q] < 0 || ordered_neighbours[q] < 0) {
				throw std::runtime_error("Failed to align CCW MPAS cell connectivity");
			}
		}
		cell.edges = std::move(ordered_edges);
		cell.neighbours = std::move(ordered_neighbours);
		cell.kite_area_m2.assign(degree, 0.0);

		// Exact spherical kite areas. For vertex q, edges[q] and edges[q+1]
		// are the two Voronoi edges meeting at that vertex. Their midpoints lie
		// on the two Delaunay sides bounding the cell/dual-triangle intersection.
		long double kite_sum = 0.0L;
		for (size_t q = 0; q < degree; ++q) {
			const int v = cell.vertices[q];
			const EdgeGeometry &before = edges_[static_cast<size_t>(cell.edges[q])];
			const EdgeGeometry &after = edges_[static_cast<size_t>(cell.edges[(q + 1) % degree])];
			const Vec3d &vertex_point = vertices_[static_cast<size_t>(v)].center;
			const double area_unit = spherical_triangle_area_unit(
				vertex_point, before.midpoint, cell.center)
				+ spherical_triangle_area_unit(vertex_point, cell.center, after.midpoint);
			const double kite = area_unit * radius_m_ * radius_m_;
			if (!(kite > 0.0) || !std::isfinite(kite)) {
				throw std::runtime_error("Invalid MPAS spherical kite area");
			}
			cell.kite_area_m2[q] = kite;
			kite_sum += static_cast<long double>(kite);
			VertexGeometry &vertex = vertices_[static_cast<size_t>(v)];
			bool stored = false;
			for (int k = 0; k < 3; ++k) {
				if (vertex.cells[k] == c) {
					vertex.kite_area_m2[k] = kite;
					stored = true;
					break;
				}
			}
			if (!stored) throw std::runtime_error("Kite cell is absent from dual vertex");
		}
		const double cell_kite_error = std::abs(static_cast<double>(kite_sum) - cell.area_m2)
			/ std::max(cell.area_m2, 1.0);
		if (cell_kite_error > 2.0e-10) {
			throw std::runtime_error("MPAS kite areas do not partition Voronoi cell");
		}
	}

	for (const VertexGeometry &vertex : vertices_) {
		long double sum = 0.0L;
		for (double kite : vertex.kite_area_m2) {
			if (!(kite > 0.0) || !std::isfinite(kite)) {
				throw std::runtime_error("Dual vertex has missing/invalid kite area");
			}
			sum += static_cast<long double>(kite);
		}
		const double error = std::abs(static_cast<double>(sum) - vertex.dual_area_m2)
			/ std::max(vertex.dual_area_m2, 1.0);
		if (error > 2.0e-10) {
			throw std::runtime_error("MPAS kite areas do not partition dual triangle");
		}
	}

	// Exact MpasMeshConverter/Thuburn weightsOnEdge construction. Around each of
	// the two target cells, start immediately CCW of the target edge, accumulate
	// kite-area fractions, and add all other cell edges in CCW order. The sign is
	// the product of target/source outward-normal signs for the shared cell.
	for (int target_id = 0; target_id < edge_count(); ++target_id) {
		EdgeGeometry &target = edges_[static_cast<size_t>(target_id)];
		const int adjacent[2] = {target.cell_a, target.cell_b};
		for (int side = 0; side < 2; ++side) {
			const int c = adjacent[side];
			const CellGeometry &cell = cells_[static_cast<size_t>(c)];
			const size_t degree = cell.edges.size();
			size_t target_pos = degree;
			for (size_t q = 0; q < degree; ++q) {
				if (cell.edges[q] == target_id) { target_pos = q; break; }
			}
			if (target_pos == degree) throw std::runtime_error("Target edge absent from adjacent cell");

			const double target_cell_sign = target.cell_a == c ? 1.0 : -1.0;
			double area_sum = 0.0;
			int last_edge = target_id;
			for (size_t step = 1; step < degree; ++step) {
				const size_t pos = (target_pos + step) % degree;
				const int source_id = cell.edges[pos];
				const EdgeGeometry &source = edges_[static_cast<size_t>(source_id)];
				const int shared = shared_vertex(edges_[static_cast<size_t>(last_edge)], source);
				if (shared < 0) throw std::runtime_error("Consecutive MPAS cell edges do not share a vertex");
				size_t shared_pos = degree;
				for (size_t q = 0; q < degree; ++q) {
					if (cell.vertices[q] == shared) { shared_pos = q; break; }
				}
				if (shared_pos == degree) throw std::runtime_error("Shared edge vertex absent from cell");
				area_sum += cell.kite_area_m2[shared_pos] / cell.area_m2;
				const double source_cell_sign = source.cell_a == c ? 1.0 : -1.0;
				const double weight = target_cell_sign * source_cell_sign
					* (0.5 - area_sum) * source.edge_length_m / target.center_distance_m;
				target.reconstruction_edges.push_back(source_id);
				target.reconstruction_weights.push_back(weight);
				last_edge = source_id;
			}
		}
		if (target.reconstruction_edges.size() != target.reconstruction_weights.size()) {
			throw std::runtime_error("MPAS reconstruction topology/weight size mismatch");
		}
		const size_t expected = cells_[static_cast<size_t>(target.cell_a)].edges.size()
			+ cells_[static_cast<size_t>(target.cell_b)].edges.size() - 2;
		if (target.reconstruction_edges.size() != expected) {
			throw std::runtime_error("MPAS edgesOnEdge count is inconsistent with adjacent cells");
		}
	}
}

} // namespace asterra::weather

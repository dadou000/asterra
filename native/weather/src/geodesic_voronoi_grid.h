#pragma once

#include "spherical_math.h"

#include <array>
#include <vector>

namespace asterra::weather {

// MPAS/TRiSK-style spherical primal/dual mesh built from an icosahedral
// geodesic triangulation. Prognostic scalar cells are spherical Voronoi regions
// around the Delaunay vertices; normal velocity lives on Voronoi edges; relative
// vorticity lives at Voronoi vertices / Delaunay-triangle circumcentres.
//
// The primal centre-to-centre line and its dual Voronoi edge are orthogonal by
// construction. There is no global latitude/longitude coordinate and no pole.
class GeodesicVoronoiGrid {
public:
	struct EdgeGeometry {
		int cell_a = -1;
		int cell_b = -1;
		int vertex_a = -1;
		int vertex_b = -1;
		Vec3d midpoint;
		Vec3d normal_a_to_b;
		Vec3d tangent_a_to_b;
		double center_distance_m = 0.0; // dcEdge / d_e
		double edge_length_m = 0.0;     // dvEdge / l_e, Voronoi edge
		double edge_area_m2 = 0.0;      // d_e * l_e

		// MPAS edgesOnEdge + weightsOnEdge. The weight already includes
		// dvEdge(source)/dcEdge(target), exactly as stored by MPAS. Applying
		// sum(weight[k] * normal_value[edge[k]]) reconstructs the target-edge
		// positive tangential component (k x normal).
		std::vector<int> reconstruction_edges;
		std::vector<double> reconstruction_weights;
	};

	struct CellGeometry {
		Vec3d center;
		double area_m2 = 0.0;

		// Strictly CCW and aligned to the MPAS convention. edges[q] is the edge
		// immediately clockwise of vertices[q], i.e. the edge ending at that
		// vertex while walking CCW. neighbours[q] is across edges[q].
		// kite_area_m2[q] is P_cell intersect D_vertices[q].
		std::vector<int> vertices;
		std::vector<int> edges;
		std::vector<int> neighbours;
		std::vector<double> kite_area_m2;
	};

	struct VertexGeometry {
		Vec3d center;
		double dual_area_m2 = 0.0;
		std::array<int, 3> cells{{-1, -1, -1}};
		std::array<int, 3> edges{{-1, -1, -1}};
		std::array<double, 3> kite_area_m2{{0.0, 0.0, 0.0}};
	};

	GeodesicVoronoiGrid() = default;
	GeodesicVoronoiGrid(int frequency, double radius_m) {
		build(frequency, radius_m);
		finalize_mpas_metrics();
	}

	// Raw geodesic construction. Normal runtime/tests use the constructor above;
	// callers that invoke build() directly must follow it with finalize_mpas_metrics().
	void build(int frequency, double radius_m);
	void finalize_mpas_metrics();

	int frequency() const { return frequency_; }
	double radius_m() const { return radius_m_; }
	int cell_count() const { return static_cast<int>(cells_.size()); }
	int edge_count() const { return static_cast<int>(edges_.size()); }
	int vertex_count() const { return static_cast<int>(vertices_.size()); }

	const CellGeometry &cell(int id) const { return cells_.at(static_cast<size_t>(id)); }
	const EdgeGeometry &edge(int id) const { return edges_.at(static_cast<size_t>(id)); }
	const VertexGeometry &vertex(int id) const { return vertices_.at(static_cast<size_t>(id)); }
	const std::vector<CellGeometry> &cells() const { return cells_; }
	const std::vector<EdgeGeometry> &edges() const { return edges_; }
	const std::vector<VertexGeometry> &vertices() const { return vertices_; }

	static int expected_cell_count(int frequency) { return 10 * frequency * frequency + 2; }
	static int expected_edge_count(int frequency) { return 30 * frequency * frequency; }
	static int expected_vertex_count(int frequency) { return 20 * frequency * frequency; }

private:
	int frequency_ = 0;
	double radius_m_ = 0.0;
	std::vector<CellGeometry> cells_;
	std::vector<EdgeGeometry> edges_;
	std::vector<VertexGeometry> vertices_;
};

} // namespace asterra::weather

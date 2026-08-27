#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace asterra::weather {

struct Vec3d {
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;

	Vec3d operator+(const Vec3d &o) const { return {x + o.x, y + o.y, z + o.z}; }
	Vec3d operator-(const Vec3d &o) const { return {x - o.x, y - o.y, z - o.z}; }
	Vec3d operator*(double s) const { return {x * s, y * s, z * s}; }
	Vec3d operator/(double s) const { return {x / s, y / s, z / s}; }
};

double dot(const Vec3d &a, const Vec3d &b);
Vec3d cross(const Vec3d &a, const Vec3d &b);
double length(const Vec3d &v);
Vec3d normalized(const Vec3d &v);

class CubedSphereGrid {
public:
	enum Face : int {
		POS_X = 0,
		NEG_X = 1,
		POS_Y = 2,
		NEG_Y = 3,
		POS_Z = 4,
		NEG_Z = 5,
		FACE_COUNT = 6,
	};

	enum Edge : int {
		WEST = 0,
		EAST = 1,
		SOUTH = 2,
		NORTH = 3,
		EDGE_COUNT = 4,
	};

	struct CellAddress {
		int face = 0;
		int i = 0;
		int j = 0;
	};

	struct Neighbour {
		int cell = -1;
		int edge = -1;
	};

	struct CellGeometry {
		Vec3d center;
		Vec3d tangent_u;
		Vec3d tangent_v;
		double area_m2 = 0.0;
		std::array<double, EDGE_COUNT> edge_length_m{};
		// Unit-sphere midpoint of each physical edge. Keeping this explicit lets
		// the finite-volume core evaluate one velocity/flux for a shared edge,
		// including edges that cross cube-face seams.
		std::array<Vec3d, EDGE_COUNT> edge_midpoint{};
		std::array<Vec3d, EDGE_COUNT> outward_normal{};
		std::array<Neighbour, EDGE_COUNT> neighbour{};
	};

	CubedSphereGrid() = default;
	CubedSphereGrid(int resolution, double radius_m) { build(resolution, radius_m); }

	void build(int resolution, double radius_m);

	int resolution() const { return resolution_; }
	double radius_m() const { return radius_m_; }
	int cells_per_face() const { return resolution_ * resolution_; }
	int cell_count() const { return FACE_COUNT * cells_per_face(); }

	int cell_id(int face, int i, int j) const;
	CellAddress address(int cell) const;
	const CellGeometry &cell(int id) const { return cells_.at(static_cast<size_t>(id)); }
	const std::vector<CellGeometry> &cells() const { return cells_; }

	Vec3d direction_from_face_ab(int face, double a, double b) const;
	CellAddress address_from_direction(const Vec3d &direction) const;

private:
	struct FaceBasis {
		Vec3d n;
		Vec3d u;
		Vec3d v;
	};

	static const std::array<FaceBasis, FACE_COUNT> FACE_BASIS;

	int resolution_ = 0;
	double radius_m_ = 0.0;
	std::vector<CellGeometry> cells_;

	double coord_center(int index) const;
	double coord_edge(int index) const;
	Vec3d corner_direction(int face, int i_edge, int j_edge) const;
	Vec3d edge_midpoint_direction(int face, int i, int j, int edge) const;
	std::array<Vec3d, 2> edge_endpoint_directions(int face, int i, int j, int edge) const;
	Neighbour locate_neighbour(int face, int i, int j, int edge) const;
};

} // namespace asterra::weather

#include "cubed_sphere_grid.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace asterra::weather {

static constexpr double PI = 3.141592653589793238462643383279502884;

const std::array<CubedSphereGrid::FaceBasis, CubedSphereGrid::FACE_COUNT>
	CubedSphereGrid::FACE_BASIS = {{
		{{ 1.0,  0.0,  0.0}, { 0.0,  0.0, -1.0}, {0.0, 1.0,  0.0}}, // +X
		{{-1.0,  0.0,  0.0}, { 0.0,  0.0,  1.0}, {0.0, 1.0,  0.0}}, // -X
		{{ 0.0,  1.0,  0.0}, { 1.0,  0.0,  0.0}, {0.0, 0.0, -1.0}}, // +Y
		{{ 0.0, -1.0,  0.0}, { 1.0,  0.0,  0.0}, {0.0, 0.0,  1.0}}, // -Y
		{{ 0.0,  0.0,  1.0}, { 1.0,  0.0,  0.0}, {0.0, 1.0,  0.0}}, // +Z
		{{ 0.0,  0.0, -1.0}, {-1.0,  0.0,  0.0}, {0.0, 1.0,  0.0}}, // -Z
	}};

double dot(const Vec3d &a, const Vec3d &b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3d cross(const Vec3d &a, const Vec3d &b) {
	return {
		a.y * b.z - a.z * b.y,
		a.z * b.x - a.x * b.z,
		a.x * b.y - a.y * b.x,
	};
}

double length(const Vec3d &v) {
	return std::sqrt(dot(v, v));
}

Vec3d normalized(const Vec3d &v) {
	const double l = length(v);
	if (!(l > 0.0) || !std::isfinite(l)) {
		throw std::runtime_error("Cannot normalize zero/non-finite vector");
	}
	return v / l;
}

static double spherical_triangle_area_unit(const Vec3d &a, const Vec3d &b, const Vec3d &c) {
	const double numerator = std::abs(dot(a, cross(b, c)));
	const double denominator = 1.0 + dot(a, b) + dot(b, c) + dot(c, a);
	return 2.0 * std::atan2(numerator, std::max(denominator, 0.0));
}

static double great_circle_angle(const Vec3d &a, const Vec3d &b) {
	return std::atan2(length(cross(a, b)), std::clamp(dot(a, b), -1.0, 1.0));
}

void CubedSphereGrid::build(int resolution, double radius_m) {
	if (resolution < 2) {
		throw std::invalid_argument("CubedSphereGrid resolution must be >= 2");
	}
	if (!(radius_m > 0.0) || !std::isfinite(radius_m)) {
		throw std::invalid_argument("CubedSphereGrid radius must be finite and positive");
	}

	resolution_ = resolution;
	radius_m_ = radius_m;
	cells_.assign(static_cast<size_t>(cell_count()), {});

	for (int face = 0; face < FACE_COUNT; ++face) {
		const FaceBasis &basis = FACE_BASIS[face];
		for (int j = 0; j < resolution_; ++j) {
			for (int i = 0; i < resolution_; ++i) {
				const int id = cell_id(face, i, j);
				CellGeometry &g = cells_[static_cast<size_t>(id)];
				const double a = coord_center(i);
				const double b = coord_center(j);
				g.center = direction_from_face_ab(face, a, b);

				Vec3d tu = basis.u - g.center * dot(basis.u, g.center);
				Vec3d tv = basis.v - g.center * dot(basis.v, g.center);
				g.tangent_u = normalized(tu);
				// Gram-Schmidt so the basis is orthonormal even away from face center.
				tv = tv - g.tangent_u * dot(tv, g.tangent_u);
				g.tangent_v = normalized(tv);
				if (dot(cross(g.tangent_u, g.tangent_v), g.center) < 0.0) {
					g.tangent_v = g.tangent_v * -1.0;
				}

				const Vec3d sw = corner_direction(face, i, j);
				const Vec3d se = corner_direction(face, i + 1, j);
				const Vec3d nw = corner_direction(face, i, j + 1);
				const Vec3d ne = corner_direction(face, i + 1, j + 1);
				const double area_unit = spherical_triangle_area_unit(sw, se, ne)
					+ spherical_triangle_area_unit(sw, ne, nw);
				g.area_m2 = area_unit * radius_m_ * radius_m_;

				for (int edge = 0; edge < EDGE_COUNT; ++edge) {
					const auto endpoints = edge_endpoint_directions(face, i, j, edge);
					g.edge_length_m[edge] = great_circle_angle(endpoints[0], endpoints[1]) * radius_m_;
					const Vec3d midpoint = edge_midpoint_direction(face, i, j, edge);
					Vec3d edge_tangent = endpoints[1] - endpoints[0];
					edge_tangent = edge_tangent - midpoint * dot(edge_tangent, midpoint);
					edge_tangent = normalized(edge_tangent);
					Vec3d outward = normalized(cross(edge_tangent, midpoint));
					Vec3d toward_edge = midpoint - g.center * dot(midpoint, g.center);
					if (dot(outward, toward_edge) < 0.0) outward = outward * -1.0;
					g.outward_normal[edge] = outward;
					g.neighbour[edge] = locate_neighbour(face, i, j, edge);
				}
			}
		}
	}

	// Resolve the reciprocal edge index after every destination cell is known.
	for (int id = 0; id < cell_count(); ++id) {
		for (int edge = 0; edge < EDGE_COUNT; ++edge) {
			Neighbour &n = cells_[static_cast<size_t>(id)].neighbour[edge];
			if (n.cell < 0 || n.cell >= cell_count()) {
				throw std::runtime_error("Invalid cubed-sphere neighbour cell");
			}
			n.edge = -1;
			for (int candidate_edge = 0; candidate_edge < EDGE_COUNT; ++candidate_edge) {
				if (cells_[static_cast<size_t>(n.cell)].neighbour[candidate_edge].cell == id) {
					n.edge = candidate_edge;
					break;
				}
			}
			if (n.edge < 0) {
				throw std::runtime_error("Cubed-sphere neighbour relation is not reciprocal");
			}
		}
	}
}

int CubedSphereGrid::cell_id(int face, int i, int j) const {
	if (face < 0 || face >= FACE_COUNT || i < 0 || i >= resolution_ || j < 0 || j >= resolution_) {
		throw std::out_of_range("CubedSphereGrid cell address out of range");
	}
	return face * cells_per_face() + j * resolution_ + i;
}

CubedSphereGrid::CellAddress CubedSphereGrid::address(int cell) const {
	if (cell < 0 || cell >= cell_count()) throw std::out_of_range("CubedSphereGrid cell id out of range");
	const int cpf = cells_per_face();
	CellAddress out;
	out.face = cell / cpf;
	const int local = cell % cpf;
	out.i = local % resolution_;
	out.j = local / resolution_;
	return out;
}

double CubedSphereGrid::coord_center(int index) const {
	return -1.0 + (static_cast<double>(index) + 0.5) * (2.0 / static_cast<double>(resolution_));
}

double CubedSphereGrid::coord_edge(int index) const {
	return -1.0 + static_cast<double>(index) * (2.0 / static_cast<double>(resolution_));
}

Vec3d CubedSphereGrid::direction_from_face_ab(int face, double a, double b) const {
	if (face < 0 || face >= FACE_COUNT) throw std::out_of_range("CubedSphereGrid face out of range");
	const FaceBasis &basis = FACE_BASIS[face];
	return normalized(basis.n + basis.u * a + basis.v * b);
}

CubedSphereGrid::CellAddress CubedSphereGrid::address_from_direction(const Vec3d &direction) const {
	const Vec3d d = normalized(direction);
	const double ax = std::abs(d.x);
	const double ay = std::abs(d.y);
	const double az = std::abs(d.z);
	int face = POS_X;
	if (ax >= ay && ax >= az) face = d.x >= 0.0 ? POS_X : NEG_X;
	else if (ay >= ax && ay >= az) face = d.y >= 0.0 ? POS_Y : NEG_Y;
	else face = d.z >= 0.0 ? POS_Z : NEG_Z;

	const FaceBasis &basis = FACE_BASIS[face];
	const double denom = dot(d, basis.n);
	if (!(denom > 0.0)) throw std::runtime_error("Invalid cubed-sphere face projection");
	const double a = dot(d, basis.u) / denom;
	const double b = dot(d, basis.v) / denom;
	const double u = (a + 1.0) * 0.5 * static_cast<double>(resolution_);
	const double v = (b + 1.0) * 0.5 * static_cast<double>(resolution_);
	CellAddress out;
	out.face = face;
	out.i = std::clamp(static_cast<int>(std::floor(u)), 0, resolution_ - 1);
	out.j = std::clamp(static_cast<int>(std::floor(v)), 0, resolution_ - 1);
	return out;
}

Vec3d CubedSphereGrid::corner_direction(int face, int i_edge, int j_edge) const {
	return direction_from_face_ab(face, coord_edge(i_edge), coord_edge(j_edge));
}

Vec3d CubedSphereGrid::edge_midpoint_direction(int face, int i, int j, int edge) const {
	const double a0 = coord_edge(i);
	const double a1 = coord_edge(i + 1);
	const double b0 = coord_edge(j);
	const double b1 = coord_edge(j + 1);
	switch (edge) {
		case WEST:  return direction_from_face_ab(face, a0, 0.5 * (b0 + b1));
		case EAST:  return direction_from_face_ab(face, a1, 0.5 * (b0 + b1));
		case SOUTH: return direction_from_face_ab(face, 0.5 * (a0 + a1), b0);
		case NORTH: return direction_from_face_ab(face, 0.5 * (a0 + a1), b1);
		default: throw std::out_of_range("CubedSphereGrid edge out of range");
	}
}

std::array<Vec3d, 2> CubedSphereGrid::edge_endpoint_directions(int face, int i, int j, int edge) const {
	switch (edge) {
		case WEST:  return {corner_direction(face, i, j), corner_direction(face, i, j + 1)};
		case EAST:  return {corner_direction(face, i + 1, j + 1), corner_direction(face, i + 1, j)};
		case SOUTH: return {corner_direction(face, i + 1, j), corner_direction(face, i, j)};
		case NORTH: return {corner_direction(face, i, j + 1), corner_direction(face, i + 1, j + 1)};
		default: throw std::out_of_range("CubedSphereGrid edge out of range");
	}
}

CubedSphereGrid::Neighbour CubedSphereGrid::locate_neighbour(int face, int i, int j, int edge) const {
	int ni = i;
	int nj = j;
	switch (edge) {
		case WEST: --ni; break;
		case EAST: ++ni; break;
		case SOUTH: --nj; break;
		case NORTH: ++nj; break;
		default: throw std::out_of_range("CubedSphereGrid edge out of range");
	}
	if (ni >= 0 && ni < resolution_ && nj >= 0 && nj < resolution_) {
		return {cell_id(face, ni, nj), -1};
	}

	const double a = coord_center(ni);
	const double b = coord_center(nj);
	const Vec3d direction = direction_from_face_ab(face, a, b);
	const CellAddress dst = address_from_direction(direction);
	return {cell_id(dst.face, dst.i, dst.j), -1};
}

} // namespace asterra::weather

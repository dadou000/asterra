#include "geodesic_voronoi_grid.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace asterra::weather {

namespace {

struct QuantizedDirection {
	std::int64_t x = 0;
	std::int64_t y = 0;
	std::int64_t z = 0;
	bool operator==(const QuantizedDirection &o) const {
		return x == o.x && y == o.y && z == o.z;
	}
};

struct QuantizedDirectionHash {
	size_t operator()(const QuantizedDirection &k) const noexcept {
		auto mix = [](std::uint64_t x) {
			x ^= x >> 33;
			x *= 0xff51afd7ed558ccdULL;
			x ^= x >> 33;
			x *= 0xc4ceb9fe1a85ec53ULL;
			x ^= x >> 33;
			return x;
		};
		const std::uint64_t a = mix(static_cast<std::uint64_t>(k.x));
		const std::uint64_t b = mix(static_cast<std::uint64_t>(k.y));
		const std::uint64_t c = mix(static_cast<std::uint64_t>(k.z));
		return static_cast<size_t>(a ^ (b << 1) ^ (c << 7));
	}
};

QuantizedDirection quantize(const Vec3d &p) {
	constexpr double SCALE = 1.0e11;
	return {
		static_cast<std::int64_t>(std::llround(p.x * SCALE)),
		static_cast<std::int64_t>(std::llround(p.y * SCALE)),
		static_cast<std::int64_t>(std::llround(p.z * SCALE)),
	};
}

std::uint64_t edge_key(int a, int b) {
	const std::uint32_t lo = static_cast<std::uint32_t>(std::min(a, b));
	const std::uint32_t hi = static_cast<std::uint32_t>(std::max(a, b));
	return (static_cast<std::uint64_t>(lo) << 32) | static_cast<std::uint64_t>(hi);
}

struct Triangle {
	std::array<int, 3> cell{{-1, -1, -1}};
};

struct EdgeBuild {
	int cell_a = -1;
	int cell_b = -1;
	int triangle_a = -1;
	int triangle_b = -1;
};

Vec3d spherical_circumcenter(const Vec3d &a, const Vec3d &b, const Vec3d &c) {
	Vec3d n = normalized(cross(b - a, c - a));
	if (dot(n, a + b + c) < 0.0) n = n * -1.0;
	return n;
}

} // namespace

void GeodesicVoronoiGrid::build(int frequency, double radius_m) {
	if (frequency < 1) throw std::invalid_argument("GeodesicVoronoiGrid frequency must be >= 1");
	if (!(radius_m > 0.0) || !std::isfinite(radius_m)) {
		throw std::invalid_argument("GeodesicVoronoiGrid radius must be finite and positive");
	}
	frequency_ = frequency;
	radius_m_ = radius_m;
	cells_.clear();
	edges_.clear();
	vertices_.clear();

	const double phi = 0.5 * (1.0 + std::sqrt(5.0));
	std::array<Vec3d, 12> base = {{
		{-1.0,  phi, 0.0}, { 1.0,  phi, 0.0}, {-1.0, -phi, 0.0}, { 1.0, -phi, 0.0},
		{0.0, -1.0,  phi}, {0.0,  1.0,  phi}, {0.0, -1.0, -phi}, {0.0,  1.0, -phi},
		{ phi, 0.0, -1.0}, { phi, 0.0,  1.0}, {-phi, 0.0, -1.0}, {-phi, 0.0,  1.0},
	}};
	for (Vec3d &v : base) v = normalized(v);

	static constexpr std::array<std::array<int, 3>, 20> BASE_FACES = {{
		{{0,11,5}}, {{0,5,1}}, {{0,1,7}}, {{0,7,10}}, {{0,10,11}},
		{{1,5,9}}, {{5,11,4}}, {{11,10,2}}, {{10,7,6}}, {{7,1,8}},
		{{3,9,4}}, {{3,4,2}}, {{3,2,6}}, {{3,6,8}}, {{3,8,9}},
		{{4,9,5}}, {{2,4,11}}, {{6,2,10}}, {{8,6,7}}, {{9,8,1}},
	}};

	std::vector<Vec3d> cell_centers;
	cell_centers.reserve(static_cast<size_t>(expected_cell_count(frequency)));
	std::unordered_map<QuantizedDirection, int, QuantizedDirectionHash> point_map;
	point_map.reserve(static_cast<size_t>(expected_cell_count(frequency) * 1.15));
	std::vector<Triangle> triangles;
	triangles.reserve(static_cast<size_t>(expected_vertex_count(frequency)));

	const int stride = frequency + 1;
	for (const auto &face : BASE_FACES) {
		const Vec3d &a = base[face[0]];
		const Vec3d &b = base[face[1]];
		const Vec3d &c = base[face[2]];
		std::vector<int> local(static_cast<size_t>(stride * stride), -1);
		auto local_index = [stride](int i, int j) { return i * stride + j; };

		for (int i = 0; i <= frequency; ++i) {
			for (int j = 0; j <= frequency - i; ++j) {
				const int k = frequency - i - j;
				const Vec3d p = normalized((a * static_cast<double>(k)
					+ b * static_cast<double>(i) + c * static_cast<double>(j))
					/ static_cast<double>(frequency));
				const QuantizedDirection key = quantize(p);
				auto it = point_map.find(key);
				int id = -1;
				if (it == point_map.end()) {
					id = static_cast<int>(cell_centers.size());
					point_map.emplace(key, id);
					cell_centers.push_back(p);
				} else {
					id = it->second;
					if (great_circle_angle(cell_centers[static_cast<size_t>(id)], p) > 1.0e-9) {
						throw std::runtime_error("Geodesic vertex quantization collision");
					}
				}
				local[static_cast<size_t>(local_index(i, j))] = id;
			}
		}

		auto add_triangle = [&](int v0, int v1, int v2) {
			Vec3d pa = cell_centers[static_cast<size_t>(v0)];
			Vec3d pb = cell_centers[static_cast<size_t>(v1)];
			Vec3d pc = cell_centers[static_cast<size_t>(v2)];
			if (dot(cross(pb - pa, pc - pa), pa + pb + pc) < 0.0) std::swap(v1, v2);
			triangles.push_back({{{v0, v1, v2}}});
		};

		for (int i = 0; i < frequency; ++i) {
			for (int j = 0; j < frequency - i; ++j) {
				const int v00 = local[static_cast<size_t>(local_index(i, j))];
				const int v10 = local[static_cast<size_t>(local_index(i + 1, j))];
				const int v01 = local[static_cast<size_t>(local_index(i, j + 1))];
				add_triangle(v00, v10, v01);
				if (i + j <= frequency - 2) {
					const int v11 = local[static_cast<size_t>(local_index(i + 1, j + 1))];
					add_triangle(v10, v11, v01);
				}
			}
		}
	}

	if (static_cast<int>(cell_centers.size()) != expected_cell_count(frequency)) {
		throw std::runtime_error("Geodesic cell count does not match icosahedral topology");
	}
	if (static_cast<int>(triangles.size()) != expected_vertex_count(frequency)) {
		throw std::runtime_error("Geodesic triangle count does not match icosahedral topology");
	}

	std::vector<Vec3d> circumcenters(triangles.size());
	std::vector<double> triangle_area_m2(triangles.size(), 0.0);
	std::vector<std::vector<int>> incident_triangles(cell_centers.size());
	for (size_t t = 0; t < triangles.size(); ++t) {
		const auto &tri = triangles[t].cell;
		const Vec3d &a = cell_centers[static_cast<size_t>(tri[0])];
		const Vec3d &b = cell_centers[static_cast<size_t>(tri[1])];
		const Vec3d &c = cell_centers[static_cast<size_t>(tri[2])];
		circumcenters[t] = spherical_circumcenter(a, b, c);
		triangle_area_m2[t] = spherical_triangle_area_unit(a, b, c) * radius_m_ * radius_m_;
		for (int v : tri) incident_triangles[static_cast<size_t>(v)].push_back(static_cast<int>(t));
	}

	std::unordered_map<std::uint64_t, int> edge_map;
	edge_map.reserve(static_cast<size_t>(expected_edge_count(frequency) * 1.15));
	std::vector<EdgeBuild> edge_build;
	edge_build.reserve(static_cast<size_t>(expected_edge_count(frequency)));
	for (size_t t = 0; t < triangles.size(); ++t) {
		const auto &tri = triangles[t].cell;
		for (int q = 0; q < 3; ++q) {
			const int x = tri[q];
			const int y = tri[(q + 1) % 3];
			const std::uint64_t key = edge_key(x, y);
			auto it = edge_map.find(key);
			if (it == edge_map.end()) {
				EdgeBuild eb;
				eb.cell_a = std::min(x, y);
				eb.cell_b = std::max(x, y);
				eb.triangle_a = static_cast<int>(t);
				const int eid = static_cast<int>(edge_build.size());
				edge_build.push_back(eb);
				edge_map.emplace(key, eid);
			} else {
				EdgeBuild &eb = edge_build[static_cast<size_t>(it->second)];
				if (eb.triangle_b >= 0) throw std::runtime_error("Geodesic edge belongs to more than two triangles");
				eb.triangle_b = static_cast<int>(t);
			}
		}
	}
	if (static_cast<int>(edge_build.size()) != expected_edge_count(frequency)) {
		throw std::runtime_error("Geodesic edge count does not match Euler topology");
	}

	cells_.resize(cell_centers.size());
	for (size_t c = 0; c < cell_centers.size(); ++c) cells_[c].center = cell_centers[c];
	edges_.resize(edge_build.size());
	vertices_.resize(triangles.size());

	for (size_t t = 0; t < triangles.size(); ++t) {
		VertexGeometry &vg = vertices_[t];
		vg.center = circumcenters[t];
		vg.dual_area_m2 = triangle_area_m2[t];
		vg.cells = triangles[t].cell;
		for (int q = 0; q < 3; ++q) {
			const int a = vg.cells[q];
			const int b = vg.cells[(q + 1) % 3];
			vg.edges[q] = edge_map.at(edge_key(a, b));
		}
	}

	for (size_t e = 0; e < edge_build.size(); ++e) {
		const EdgeBuild &eb = edge_build[e];
		if (eb.triangle_a < 0 || eb.triangle_b < 0) throw std::runtime_error("Open edge in closed geodesic sphere");
		EdgeGeometry &g = edges_[e];
		g.cell_a = eb.cell_a;
		g.cell_b = eb.cell_b;
		g.vertex_a = eb.triangle_a;
		g.vertex_b = eb.triangle_b;
		const Vec3d &ca = cell_centers[static_cast<size_t>(g.cell_a)];
		const Vec3d &cb = cell_centers[static_cast<size_t>(g.cell_b)];
		const Vec3d &va = circumcenters[static_cast<size_t>(g.vertex_a)];
		const Vec3d &vb = circumcenters[static_cast<size_t>(g.vertex_b)];
		g.midpoint = normalized(ca + cb);
		g.normal_a_to_b = normalized(project_tangent(cb, g.midpoint));
		Vec3d tangent = normalized(project_tangent(vb - va, g.midpoint));
		if (dot(tangent, vb - va) < 0.0) tangent = tangent * -1.0;
		g.tangent_a_to_b = tangent;
		g.center_distance_m = great_circle_angle(ca, cb) * radius_m_;
		g.edge_length_m = great_circle_angle(va, vb) * radius_m_;
		g.edge_area_m2 = g.center_distance_m * g.edge_length_m;
		if (!(g.center_distance_m > 0.0) || !(g.edge_length_m > 0.0)
				|| !std::isfinite(g.edge_area_m2)) {
			throw std::runtime_error("Invalid geodesic primal/dual edge metric");
		}
		cells_[static_cast<size_t>(g.cell_a)].edges.push_back(static_cast<int>(e));
		cells_[static_cast<size_t>(g.cell_a)].neighbours.push_back(g.cell_b);
		cells_[static_cast<size_t>(g.cell_b)].edges.push_back(static_cast<int>(e));
		cells_[static_cast<size_t>(g.cell_b)].neighbours.push_back(g.cell_a);
	}

	// Order circumcentres around each Delaunay vertex, forming the corresponding
	// Voronoi polygon, then integrate its exact spherical area as a triangle fan.
	for (size_t c = 0; c < cells_.size(); ++c) {
		CellGeometry &cell = cells_[c];
		const Vec3d radial = cell.center;
		const Vec3d reference = std::abs(radial.y) < 0.9 ? Vec3d{0.0, 1.0, 0.0} : Vec3d{1.0, 0.0, 0.0};
		const Vec3d e1 = normalized(cross(reference, radial));
		const Vec3d e2 = normalized(cross(radial, e1));
		std::vector<std::pair<double, int>> ordered;
		ordered.reserve(incident_triangles[c].size());
		for (int t : incident_triangles[c]) {
			Vec3d d = project_tangent(circumcenters[static_cast<size_t>(t)], radial);
			d = normalized(d);
			ordered.emplace_back(std::atan2(dot(d, e2), dot(d, e1)), t);
		}
		std::sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
		cell.vertices.clear();
		cell.vertices.reserve(ordered.size());
		for (const auto &item : ordered) cell.vertices.push_back(item.second);
		if (cell.vertices.size() < 5 || cell.vertices.size() > 6) {
			throw std::runtime_error("Unexpected Voronoi cell degree on geodesic sphere");
		}
		double area_unit = 0.0;
		for (size_t q = 0; q < cell.vertices.size(); ++q) {
			const Vec3d &v0 = circumcenters[static_cast<size_t>(cell.vertices[q])];
			const Vec3d &v1 = circumcenters[static_cast<size_t>(cell.vertices[(q + 1) % cell.vertices.size()])];
			area_unit += spherical_triangle_area_unit(radial, v0, v1);
		}
		cell.area_m2 = area_unit * radius_m_ * radius_m_;
		if (!(cell.area_m2 > 0.0) || !std::isfinite(cell.area_m2)) {
			throw std::runtime_error("Invalid spherical Voronoi cell area");
		}
	}
}

} // namespace asterra::weather

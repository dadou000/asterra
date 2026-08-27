#pragma once

#include <cmath>
#include <stdexcept>

namespace asterra::weather {

struct Vec3d {
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;

	Vec3d operator+(const Vec3d &o) const { return {x + o.x, y + o.y, z + o.z}; }
	Vec3d operator-(const Vec3d &o) const { return {x - o.x, y - o.y, z - o.z}; }
	Vec3d operator*(double s) const { return {x * s, y * s, z * s}; }
	Vec3d operator/(double s) const { return {x / s, y / s, z / s}; }
	Vec3d &operator+=(const Vec3d &o) { x += o.x; y += o.y; z += o.z; return *this; }
};

inline double dot(const Vec3d &a, const Vec3d &b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3d cross(const Vec3d &a, const Vec3d &b) {
	return {
		a.y * b.z - a.z * b.y,
		a.z * b.x - a.x * b.z,
		a.x * b.y - a.y * b.x,
	};
}

inline double length(const Vec3d &v) {
	return std::sqrt(dot(v, v));
}

inline Vec3d normalized(const Vec3d &v) {
	const double l = length(v);
	if (!(l > 0.0) || !std::isfinite(l)) {
		throw std::runtime_error("Cannot normalize zero/non-finite vector");
	}
	return v / l;
}

inline Vec3d project_tangent(const Vec3d &v, const Vec3d &unit_radial) {
	return v - unit_radial * dot(v, unit_radial);
}

inline double great_circle_angle(const Vec3d &a, const Vec3d &b) {
	return std::atan2(length(cross(a, b)), dot(a, b));
}

inline double spherical_triangle_area_unit(const Vec3d &a, const Vec3d &b, const Vec3d &c) {
	const double numerator = std::abs(dot(a, cross(b, c)));
	const double denominator = 1.0 + dot(a, b) + dot(b, c) + dot(c, a);
	return 2.0 * std::atan2(numerator, std::max(denominator, 0.0));
}

} // namespace asterra::weather

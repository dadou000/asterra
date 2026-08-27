#include "conservative_transport.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using asterra::weather::ConservativeTransport2D;
using asterra::weather::CubedSphereGrid;
using asterra::weather::Vec3d;
using asterra::weather::cross;
using asterra::weather::dot;
using asterra::weather::length;
using asterra::weather::normalized;

static constexpr double PI = 3.141592653589793238462643383279502884;

static void require(bool condition, const char *message) {
	if (!condition) throw std::runtime_error(message);
}

static double angular_distance(const Vec3d &a, const Vec3d &b) {
	return std::atan2(length(cross(a, b)), std::clamp(dot(a, b), -1.0, 1.0));
}

static Vec3d tracer_centroid(const CubedSphereGrid &grid, const std::vector<double> &q) {
	Vec3d sum{};
	for (int c = 0; c < grid.cell_count(); ++c) {
		const double weight = q[c] * grid.cell(c).area_m2;
		sum = sum + grid.cell(c).center * weight;
	}
	return normalized(sum);
}

int main() {
	try {
		constexpr int N = 32;
		constexpr double R = 3500000.0;
		CubedSphereGrid grid(N, R);
		ConservativeTransport2D transport(grid);
		require(transport.shared_edges().size() == static_cast<size_t>(2 * grid.cell_count()),
			"wrong unique shared-edge count");

		// Gate 1: a zero-flow constant field must remain bitwise unchanged. This is
		// deliberately strict: numerical stabilization is not allowed to modify a
		// state when there is no physical flux.
		std::vector<double> zero_velocity(transport.shared_edges().size(), 0.0);
		std::vector<double> constant(static_cast<size_t>(grid.cell_count()), 3.25);
		const std::vector<double> constant_before = constant;
		auto zero_diag = transport.step_ssprk3(constant, zero_velocity, 86400.0, 0.45);
		require(constant == constant_before, "zero-flow constant field changed");
		require(zero_diag.relative_mass_error == 0.0, "zero-flow mass changed");

		// Gate 2: arbitrary shared-edge flow still has exactly one donor and one
		// receiver. A deliberately sharp positive field exercises MUSCL + the donor
		// positivity limiter while CFL control shortens an intentionally huge dt.
		std::vector<double> edge_velocity(transport.shared_edges().size(), 0.0);
		for (size_t e = 0; e < edge_velocity.size(); ++e) {
			edge_velocity[e] = 38.0 * std::sin(0.017 * static_cast<double>(e + 1))
				+ 11.0 * std::cos(0.043 * static_cast<double>(e + 7));
		}
		std::vector<double> sharp(static_cast<size_t>(grid.cell_count()), 0.0);
		for (int c = 0; c < grid.cell_count(); ++c) {
			const double x = static_cast<double>((c * 37) % 101) / 100.0;
			sharp[c] = x > 0.72 ? (0.05 + 4.0 * (x - 0.72)) : 0.0;
		}
		const double sharp_mass0 = transport.total_mass(sharp);
		auto sharp_diag = transport.step_ssprk3(sharp, edge_velocity, 1.0e9, 0.45);
		require(sharp_diag.used_dt_s < sharp_diag.requested_dt_s, "CFL controller did not reduce an unsafe timestep");
		require(sharp_diag.max_courant <= 0.450000000001, "accepted transport step exceeded target CFL");
		require(sharp_diag.min_density >= 0.0, "transport violated positivity");
		const double sharp_mass_error = std::abs(transport.total_mass(sharp) - sharp_mass0)
			/ std::max(std::abs(sharp_mass0), 1.0);
		require(sharp_mass_error < 2e-13, "shared-edge transport does not conserve mass");

		// Gate 3: a smooth compact cosine bell is carried through a full tilted
		// solid-body revolution, crossing multiple cube seams and corners. Shape is
		// allowed truncation error; global mass and positivity are not.
		const Vec3d axis = normalized(Vec3d{0.37, 0.81, -0.45});
		const Vec3d initial_center = normalized(Vec3d{0.91, -0.24, 0.34});
		constexpr double PERIOD_S = 6.0 * 86400.0;
		const Vec3d omega = axis * (2.0 * PI / PERIOD_S);
		const auto solid_body_velocity = [omega](const Vec3d &unit_position) -> Vec3d {
			return cross(omega, unit_position) * R;
		};
		const std::vector<double> solid_edge_velocity =
			transport.sample_edge_normal_velocity(solid_body_velocity);

		std::vector<double> tracer(static_cast<size_t>(grid.cell_count()), 0.0);
		constexpr double BELL_RADIUS = 0.52;
		for (int c = 0; c < grid.cell_count(); ++c) {
			const double angle = angular_distance(grid.cell(c).center, initial_center);
			if (angle < BELL_RADIUS) {
				tracer[c] = 0.5 * (1.0 + std::cos(PI * angle / BELL_RADIUS));
			}
		}
		const std::vector<double> tracer_initial = tracer;
		const double mass0 = transport.total_mass(tracer);
		const Vec3d centroid0 = tracer_centroid(grid, tracer);

		double elapsed = 0.0;
		double worst_cfl = 0.0;
		double worst_mass_error = 0.0;
		std::size_t total_limiter_activations = 0;
		int steps = 0;
		while (elapsed < PERIOD_S) {
			const double request = std::min(3600.0, PERIOD_S - elapsed);
			auto diag = transport.step_ssprk3(tracer, solid_edge_velocity, request, 0.40);
			require(diag.used_dt_s > 0.0, "transport returned a zero timestep");
			require(diag.max_courant <= 0.400000000001, "solid-body step exceeded target CFL");
			worst_cfl = std::max(worst_cfl, diag.max_courant);
			worst_mass_error = std::max(worst_mass_error, diag.relative_mass_error);
			total_limiter_activations += diag.positivity_limiter_activations;
			elapsed += diag.used_dt_s;
			++steps;
			require(steps < 10000, "solid-body test timestep collapsed");
		}

		const double mass1 = transport.total_mass(tracer);
		const double full_rotation_mass_error = std::abs(mass1 - mass0) / std::max(std::abs(mass0), 1.0);
		require(full_rotation_mass_error < 1e-11, "mass drifted during full solid-body revolution");
		require(*std::min_element(tracer.begin(), tracer.end()) >= 0.0,
			"solid-body transport produced negative tracer");

		long double l1_numerator = 0.0L;
		long double l1_denominator = 0.0L;
		for (int c = 0; c < grid.cell_count(); ++c) {
			const long double area = static_cast<long double>(grid.cell(c).area_m2);
			l1_numerator += std::abs(static_cast<long double>(tracer[c] - tracer_initial[c])) * area;
			l1_denominator += std::abs(static_cast<long double>(tracer_initial[c])) * area;
		}
		const double relative_l1_shape_error = static_cast<double>(
			l1_numerator / std::max(l1_denominator, 1.0L));
		const Vec3d centroid1 = tracer_centroid(grid, tracer);
		const double centroid_return_error = angular_distance(centroid0, centroid1);

		// These are deliberately loose first-generation accuracy gates. Conservation
		// and positivity are strict now; the shape threshold will tighten when the
		// reconstruction is upgraded after the shallow-water core is operational.
		require(relative_l1_shape_error < 0.38, "solid-body tracer is excessively diffusive/distorted");
		require(centroid_return_error < 0.10, "solid-body tracer did not return across cube seams");

		std::cout << "ConservativeTransport2D PASS\n"
			<< "  cells: " << grid.cell_count() << "\n"
			<< "  shared physical edges: " << transport.shared_edges().size() << "\n"
			<< "  sharp-field mass error: " << sharp_mass_error << "\n"
			<< "  solid-body steps: " << steps << "\n"
			<< "  worst CFL: " << worst_cfl << "\n"
			<< "  worst per-step mass error: " << worst_mass_error << "\n"
			<< "  full-rotation mass error: " << full_rotation_mass_error << "\n"
			<< "  relative L1 shape error: " << relative_l1_shape_error << "\n"
			<< "  centroid return error rad: " << centroid_return_error << "\n"
			<< "  positivity limiter activations: " << total_limiter_activations << "\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &e) {
		std::cerr << "ConservativeTransport2D FAIL: " << e.what() << "\n";
		return EXIT_FAILURE;
	}
}

#pragma once

#include "conservative_transport.h"

#include <array>
#include <cstddef>
#include <vector>

namespace asterra::weather {

// Pole-free finite-volume C-grid shallow-water core on the cubed sphere.
// Layer depth/geopotential is cell-centered and one normal velocity lives on
// every physical shared edge. The pressure operator keeps the direct cell jump
// and applies a non-orthogonal metric correction, so cube skew does not corrupt
// geostrophic balance and a 2-delta-x checkerboard cannot become a null mode.
class ShallowWaterCGrid {
public:
	struct State {
		std::vector<double> depth_m;
		std::vector<double> edge_normal_mps;
	};

	struct StepDiagnostics {
		double requested_dt_s = 0.0;
		double accepted_dt_s = 0.0;
		double max_wave_courant = 0.0;
		double mass_before_m3 = 0.0;
		double mass_after_m3 = 0.0;
		double relative_mass_error = 0.0;
		double min_depth_m = 0.0;
		double max_depth_m = 0.0;
		double max_speed_mps = 0.0;
		std::size_t positivity_limiter_activations = 0;
		int rejected_steps = 0;
	};

	ShallowWaterCGrid(const CubedSphereGrid &grid,
		double gravity_mps2 = 9.80665,
		double rotation_rate_rad_s = 0.0,
		Vec3d rotation_axis = {0.0, 1.0, 0.0});

	State make_uniform_state(double depth_m) const;
	const ConservativeTransport2D &topology() const { return topology_; }
	double gravity_mps2() const { return gravity_mps2_; }
	double rotation_rate_rad_s() const { return rotation_rate_rad_s_; }
	const Vec3d &rotation_axis() const { return rotation_axis_; }

	double total_volume_m3(const State &state) const;
	double max_wave_courant(const State &state, double dt_s) const;
	double stable_dt(const State &state, double target_cfl,
		double maximum_dt_s) const;

	std::vector<Vec3d> reconstruct_cell_velocity(const State &state) const;

	StepDiagnostics step(State &state, double requested_dt_s,
		double target_cfl = 0.40, int max_retries = 8) const;

private:
	struct StageDiagnostics {
		std::size_t positivity_limiter_activations = 0;
	};

	const CubedSphereGrid *grid_ = nullptr;
	ConservativeTransport2D topology_;
	double gravity_mps2_ = 9.80665;
	double rotation_rate_rad_s_ = 0.0;
	Vec3d rotation_axis_{0.0, 1.0, 0.0};

	// Dual-edge geometry. center_distance is used for the gravity-wave CFL. The
	// signed normal/tangential displacement components are used by the
	// non-orthogonal pressure-gradient correction.
	std::vector<double> edge_center_distance_m_;
	std::vector<double> edge_normal_separation_m_;
	std::vector<double> edge_tangential_offset_m_;
	std::vector<Vec3d> edge_tangent_direction_;

	std::vector<std::array<int, CubedSphereGrid::EDGE_COUNT>> cell_shared_edge_index_;
	std::vector<std::array<double, CubedSphereGrid::EDGE_COUNT>> cell_shared_edge_sign_;

	void validate_shape(const State &state) const;
	bool validate_finite_positive(const State &state) const;
	bool is_exact_rest_state(const State &state) const;

	// Least-squares cell gradient on the sphere, used only for the tangential
	// correction of a shared-edge pressure jump. The normal component always
	// retains the direct h_b-h_a difference, preserving checkerboard detection.
	std::vector<Vec3d> reconstruct_cell_scalar_gradient(
		const std::vector<double> &scalar) const;

	StageDiagnostics euler_stage(const State &input, State &output,
		double dt_s) const;
	bool ssprk3_attempt(const State &initial, State &candidate, double dt_s,
		std::size_t &positivity_limiter_activations) const;
};

} // namespace asterra::weather

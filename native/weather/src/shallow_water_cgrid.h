#pragma once

#include "conservative_transport.h"

#include <array>
#include <cstddef>
#include <vector>

namespace asterra::weather {

// First dynamical gate for the replacement atmosphere. Layer depth/geopotential
// lives at finite-volume cell centers and one normal velocity lives on each
// physical shared edge. This staggering makes an alternating cell-pressure field
// visible directly to the edge pressure-gradient operator; the old A-grid
// 2-delta-x checkerboard null space does not exist here.
class ShallowWaterCGrid {
public:
	struct State {
		std::vector<double> depth_m;              // cell centered
		std::vector<double> edge_normal_mps;      // one value per shared edge, a -> b
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

	// rotation_rate_rad_s defaults to zero so the pure gravity-wave core can be
	// tested independently. Production Asterra uses 2*pi/(11.5 h) with +Y as the
	// spin axis. Coriolis is evaluated geometrically from the edge midpoint, so
	// there is no latitude coordinate and no polar branch in the equations.
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

	// Reconstruct the tangent velocity vector at each cell center from the four
	// surrounding edge-normal degrees of freedom using a 2x2 least-squares solve.
	// This is used by spherical Coriolis now and by vorticity/kinetic-energy
	// operators in the nonlinear vector-invariant subphase.
	std::vector<Vec3d> reconstruct_cell_velocity(const State &state) const;

	// Hydrostatic shallow-water step over a flat lower boundary. Phase 2a includes
	// conservative continuity + C-grid pressure gradient. Phase 2b adds the
	// spherical Coriolis term while retaining the same positivity/CFL/rollback
	// gates. Nonlinear relative-vorticity and kinetic-energy terms are the next
	// subphase before the Rossby-Haurwitz gate.
	//
	// Every attempted SSPRK3 step is validated. A failed positivity/finite-state
	// gate rolls back to the input state and retries at half dt; no physical state
	// variable is clipped to conceal instability.
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
	std::vector<double> edge_center_distance_m_;
	std::vector<std::array<int, CubedSphereGrid::EDGE_COUNT>> cell_shared_edge_index_;
	std::vector<std::array<double, CubedSphereGrid::EDGE_COUNT>> cell_shared_edge_sign_;

	void validate_shape(const State &state) const;
	bool validate_finite_positive(const State &state) const;
	bool is_exact_rest_state(const State &state) const;

	StageDiagnostics euler_stage(const State &input, State &output,
		double dt_s) const;
	bool ssprk3_attempt(const State &initial, State &candidate, double dt_s,
		std::size_t &positivity_limiter_activations) const;
};

} // namespace asterra::weather

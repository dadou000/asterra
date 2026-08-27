#include "shallow_water_cgrid.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace asterra::weather {

static double gc_distance_m(const Vec3d &a, const Vec3d &b, double radius_m) {
	const double angle = std::atan2(length(cross(a, b)), std::clamp(dot(a, b), -1.0, 1.0));
	return angle * radius_m;
}

ShallowWaterCGrid::ShallowWaterCGrid(const CubedSphereGrid &grid, double gravity_mps2)
	: grid_(&grid), topology_(grid), gravity_mps2_(gravity_mps2) {
	if (!(gravity_mps2_ > 0.0) || !std::isfinite(gravity_mps2_)) {
		throw std::invalid_argument("ShallowWaterCGrid gravity must be finite and positive");
	}
	edge_center_distance_m_.resize(topology_.shared_edges().size());
	for (size_t e = 0; e < topology_.shared_edges().size(); ++e) {
		const auto &edge = topology_.shared_edges()[e];
		const double distance = gc_distance_m(
			grid.cell(edge.cell_a).center, grid.cell(edge.cell_b).center, grid.radius_m());
		if (!(distance > 0.0) || !std::isfinite(distance)) {
			throw std::runtime_error("ShallowWaterCGrid encountered a degenerate dual edge");
		}
		edge_center_distance_m_[e] = distance;
	}
}

ShallowWaterCGrid::State ShallowWaterCGrid::make_uniform_state(double depth_m) const {
	if (!(depth_m > 0.0) || !std::isfinite(depth_m)) {
		throw std::invalid_argument("Uniform shallow-water depth must be finite and positive");
	}
	State state;
	state.depth_m.assign(static_cast<size_t>(grid_->cell_count()), depth_m);
	state.edge_normal_mps.assign(topology_.shared_edges().size(), 0.0);
	return state;
}

void ShallowWaterCGrid::validate_shape(const State &state) const {
	if (state.depth_m.size() != static_cast<size_t>(grid_->cell_count())) {
		throw std::invalid_argument("Shallow-water depth array has the wrong size");
	}
	if (state.edge_normal_mps.size() != topology_.shared_edges().size()) {
		throw std::invalid_argument("Shallow-water edge velocity array has the wrong size");
	}
}

bool ShallowWaterCGrid::validate_finite_positive(const State &state) const {
	for (double h : state.depth_m) {
		if (!(h > 0.0) || !std::isfinite(h)) return false;
	}
	for (double u : state.edge_normal_mps) {
		if (!std::isfinite(u)) return false;
	}
	return true;
}

bool ShallowWaterCGrid::is_exact_rest_state(const State &state) const {
	if (state.depth_m.empty()) return false;
	const double h0 = state.depth_m.front();
	for (double h : state.depth_m) {
		if (h != h0) return false;
	}
	for (double u : state.edge_normal_mps) {
		if (u != 0.0) return false;
	}
	return true;
}

double ShallowWaterCGrid::total_volume_m3(const State &state) const {
	validate_shape(state);
	long double total = 0.0L;
	for (int c = 0; c < grid_->cell_count(); ++c) {
		const double h = state.depth_m[c];
		if (!std::isfinite(h)) throw std::runtime_error("Shallow-water depth contains NaN/Inf");
		total += static_cast<long double>(h)
			* static_cast<long double>(grid_->cell(c).area_m2);
	}
	return static_cast<double>(total);
}

double ShallowWaterCGrid::max_wave_courant(const State &state, double dt_s) const {
	validate_shape(state);
	if (!(dt_s >= 0.0) || !std::isfinite(dt_s)) {
		throw std::invalid_argument("Shallow-water CFL timestep must be finite and non-negative");
	}
	double max_rate = 0.0;
	for (size_t e = 0; e < topology_.shared_edges().size(); ++e) {
		const auto &edge = topology_.shared_edges()[e];
		const double h = std::max(state.depth_m[edge.cell_a], state.depth_m[edge.cell_b]);
		if (!(h > 0.0) || !std::isfinite(h) || !std::isfinite(state.edge_normal_mps[e])) {
			return std::numeric_limits<double>::infinity();
		}
		const double characteristic_speed = std::abs(state.edge_normal_mps[e])
			+ std::sqrt(gravity_mps2_ * h);
		max_rate = std::max(max_rate, characteristic_speed / edge_center_distance_m_[e]);
	}
	// The fastest 2-D C-grid gravity mode contains simultaneous propagation in
	// both horizontal directions. sqrt(2) converts the edge Courant to a safe
	// quasi-uniform two-dimensional wave Courant.
	return dt_s * max_rate * std::sqrt(2.0);
}

double ShallowWaterCGrid::stable_dt(const State &state, double target_cfl,
		double maximum_dt_s) const {
	if (!(target_cfl > 0.0) || !std::isfinite(target_cfl)) {
		throw std::invalid_argument("Shallow-water target CFL must be finite and positive");
	}
	if (!(maximum_dt_s > 0.0) || !std::isfinite(maximum_dt_s)) {
		throw std::invalid_argument("Shallow-water maximum timestep must be finite and positive");
	}
	const double unit_cfl = max_wave_courant(state, 1.0);
	if (!std::isfinite(unit_cfl) || !(unit_cfl > 0.0)) {
		throw std::runtime_error("Shallow-water state has invalid characteristic speed");
	}
	return std::min(maximum_dt_s, target_cfl / unit_cfl);
}

ShallowWaterCGrid::StageDiagnostics ShallowWaterCGrid::euler_stage(
		const State &input, State &output, double dt_s) const {
	validate_shape(input);
	if (!(dt_s > 0.0) || !std::isfinite(dt_s)) {
		throw std::invalid_argument("Shallow-water Euler timestep must be finite and positive");
	}
	if (!validate_finite_positive(input)) {
		throw std::runtime_error("Shallow-water Euler stage received an invalid state");
	}

	StageDiagnostics diagnostics;
	const size_t cell_count = static_cast<size_t>(grid_->cell_count());
	const size_t edge_count = topology_.shared_edges().size();
	output.depth_m.resize(cell_count);
	output.edge_normal_mps.resize(edge_count);

	// Continuity: one physical volume flux per shared edge. Upwind depth makes the
	// finite-volume mass equation monotone. A multidimensional donor limiter scales
	// all outgoing fluxes from a nearly dry cell together, preserving positivity
	// and exact equal/opposite transfer without post-step clipping.
	std::vector<double> raw_volume_rate(edge_count, 0.0); // m^3/s, positive a -> b
	std::vector<double> outgoing_rate(cell_count, 0.0);
	for (size_t e = 0; e < edge_count; ++e) {
		const auto &edge = topology_.shared_edges()[e];
		const double u = input.edge_normal_mps[e];
		if (u == 0.0) continue;
		const int donor = u > 0.0 ? edge.cell_a : edge.cell_b;
		const double flux = u * edge.length_m * input.depth_m[donor];
		raw_volume_rate[e] = flux;
		outgoing_rate[donor] += std::abs(flux);
	}

	std::vector<double> donor_scale(cell_count, 1.0);
	constexpr double RESERVE = 128.0 * std::numeric_limits<double>::epsilon();
	for (int c = 0; c < grid_->cell_count(); ++c) {
		const double available = input.depth_m[c] * grid_->cell(c).area_m2;
		const double requested = outgoing_rate[c] * dt_s;
		if (requested > available * (1.0 - RESERVE) && requested > 0.0) {
			donor_scale[c] = std::max(available * (1.0 - RESERVE), 0.0) / requested;
			++diagnostics.positivity_limiter_activations;
		}
	}

	std::vector<double> delta_volume(cell_count, 0.0);
	for (size_t e = 0; e < edge_count; ++e) {
		const auto &edge = topology_.shared_edges()[e];
		const double raw = raw_volume_rate[e];
		if (raw == 0.0) continue;
		const int donor = raw > 0.0 ? edge.cell_a : edge.cell_b;
		const double transfer = raw * donor_scale[donor] * dt_s;
		delta_volume[edge.cell_a] -= transfer;
		delta_volume[edge.cell_b] += transfer;
	}

	for (int c = 0; c < grid_->cell_count(); ++c) {
		const double volume = input.depth_m[c] * grid_->cell(c).area_m2 + delta_volume[c];
		if (!(volume > 0.0) || !std::isfinite(volume)) {
			throw std::runtime_error("Shallow-water continuity produced non-positive volume");
		}
		output.depth_m[c] = volume / grid_->cell(c).area_m2;
	}

	// C-grid pressure gradient: scalar geopotential lives at cell centers and its
	// difference accelerates the normal velocity that lives directly between those
	// cells. An alternating +/- cell field therefore produces a maximum, not zero,
	// edge gradient; this removes the old collocated checkerboard null mode.
	for (size_t e = 0; e < edge_count; ++e) {
		const auto &edge = topology_.shared_edges()[e];
		const double gradient = (input.depth_m[edge.cell_b] - input.depth_m[edge.cell_a])
			/ edge_center_distance_m_[e];
		output.edge_normal_mps[e] = input.edge_normal_mps[e]
			- gravity_mps2_ * gradient * dt_s;
		if (!std::isfinite(output.edge_normal_mps[e])) {
			throw std::runtime_error("Shallow-water momentum produced NaN/Inf");
		}
	}
	return diagnostics;
}

bool ShallowWaterCGrid::ssprk3_attempt(const State &initial, State &candidate, double dt_s,
		std::size_t &positivity_limiter_activations) const {
	try {
		State s1, euler2, s2, euler3;
		positivity_limiter_activations = 0;
		positivity_limiter_activations += euler_stage(initial, s1, dt_s).positivity_limiter_activations;
		positivity_limiter_activations += euler_stage(s1, euler2, dt_s).positivity_limiter_activations;

		s2.depth_m.resize(initial.depth_m.size());
		s2.edge_normal_mps.resize(initial.edge_normal_mps.size());
		for (size_t c = 0; c < initial.depth_m.size(); ++c) {
			s2.depth_m[c] = 0.75 * initial.depth_m[c] + 0.25 * euler2.depth_m[c];
		}
		for (size_t e = 0; e < initial.edge_normal_mps.size(); ++e) {
			s2.edge_normal_mps[e] = 0.75 * initial.edge_normal_mps[e]
				+ 0.25 * euler2.edge_normal_mps[e];
		}
		if (!validate_finite_positive(s2)) return false;

		positivity_limiter_activations += euler_stage(s2, euler3, dt_s).positivity_limiter_activations;
		candidate.depth_m.resize(initial.depth_m.size());
		candidate.edge_normal_mps.resize(initial.edge_normal_mps.size());
		for (size_t c = 0; c < initial.depth_m.size(); ++c) {
			candidate.depth_m[c] = (1.0 / 3.0) * initial.depth_m[c]
				+ (2.0 / 3.0) * euler3.depth_m[c];
		}
		for (size_t e = 0; e < initial.edge_normal_mps.size(); ++e) {
			candidate.edge_normal_mps[e] = (1.0 / 3.0) * initial.edge_normal_mps[e]
				+ (2.0 / 3.0) * euler3.edge_normal_mps[e];
		}
		return validate_finite_positive(candidate);
	} catch (const std::exception &) {
		return false;
	}
}

ShallowWaterCGrid::StepDiagnostics ShallowWaterCGrid::step(
		State &state, double requested_dt_s, double target_cfl, int max_retries) const {
	validate_shape(state);
	if (!(requested_dt_s > 0.0) || !std::isfinite(requested_dt_s)) {
		throw std::invalid_argument("Shallow-water requested timestep must be finite and positive");
	}
	if (!(target_cfl > 0.0) || target_cfl > 0.9 || !std::isfinite(target_cfl)) {
		throw std::invalid_argument("Shallow-water target CFL must be in (0, 0.9]");
	}
	if (max_retries < 0) throw std::invalid_argument("Shallow-water max_retries must be non-negative");
	if (!validate_finite_positive(state)) {
		throw std::runtime_error("Shallow-water step received an invalid state");
	}

	StepDiagnostics diag;
	diag.requested_dt_s = requested_dt_s;
	diag.mass_before_m3 = total_volume_m3(state);
	const State initial = state;
	const double first_dt = stable_dt(initial, target_cfl, requested_dt_s);

	// A perfectly uniform resting layer has exactly zero RHS. Preserve it bitwise;
	// still report the CFL-limited accepted interval so the scheduler never claims
	// an explicit step larger than its gravity-wave stability envelope.
	if (is_exact_rest_state(initial)) {
		diag.accepted_dt_s = first_dt;
		diag.max_wave_courant = max_wave_courant(initial, first_dt);
		diag.mass_after_m3 = diag.mass_before_m3;
		diag.relative_mass_error = 0.0;
		diag.min_depth_m = initial.depth_m.front();
		diag.max_depth_m = initial.depth_m.front();
		diag.max_speed_mps = 0.0;
		return diag;
	}

	double trial_dt = first_dt;
	State candidate;
	std::size_t limiter_count = 0;
	bool accepted = false;
	for (int attempt = 0; attempt <= max_retries; ++attempt) {
		if (ssprk3_attempt(initial, candidate, trial_dt, limiter_count)) {
			const double trial_cfl = max_wave_courant(candidate, trial_dt);
			if (std::isfinite(trial_cfl) && trial_cfl <= target_cfl * 1.05) {
				accepted = true;
				break;
			}
		}
		++diag.rejected_steps;
		trial_dt *= 0.5;
		if (!(trial_dt > 1.0e-6)) break;
	}
	if (!accepted) {
		state = initial;
		throw std::runtime_error("Shallow-water timestep failed invariants after rollback retries");
	}

	state = std::move(candidate);
	diag.accepted_dt_s = trial_dt;
	diag.max_wave_courant = max_wave_courant(state, trial_dt);
	diag.positivity_limiter_activations = limiter_count;
	diag.mass_after_m3 = total_volume_m3(state);
	diag.relative_mass_error = std::abs(diag.mass_after_m3 - diag.mass_before_m3)
		/ std::max(std::abs(diag.mass_before_m3), 1.0);
	diag.min_depth_m = std::numeric_limits<double>::infinity();
	diag.max_depth_m = -std::numeric_limits<double>::infinity();
	diag.max_speed_mps = 0.0;
	for (double h : state.depth_m) {
		diag.min_depth_m = std::min(diag.min_depth_m, h);
		diag.max_depth_m = std::max(diag.max_depth_m, h);
	}
	for (double u : state.edge_normal_mps) {
		diag.max_speed_mps = std::max(diag.max_speed_mps, std::abs(u));
	}
	return diag;
}

} // namespace asterra::weather

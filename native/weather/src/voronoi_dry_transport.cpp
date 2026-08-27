#include "voronoi_dry_transport.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace asterra::weather {

namespace {
constexpr double CONSERVATION_REJECT_TOL = 1.0e-11;
}

VoronoiDryTransport::VoronoiDryTransport(const GeodesicVoronoiGrid &grid,
		double gravity_mps2, double scale_height_m)
	: grid_(&grid), hydrostatic_(grid, gravity_mps2, scale_height_m) {
	if (grid.cell_count() <= 0 || grid.edge_count() <= 0) {
		throw std::invalid_argument("VoronoiDryTransport requires a built geodesic grid");
	}
}

void VoronoiDryTransport::validate_shape(const State &state) const {
	const size_t scalar_count = static_cast<size_t>(LEVELS) * static_cast<size_t>(grid_->cell_count());
	const size_t edge_count = static_cast<size_t>(LEVELS) * static_cast<size_t>(grid_->edge_count());
	if (state.layer_mass_kg_m2.size() != scalar_count) {
		throw std::invalid_argument("Dry-transport layer-mass array has wrong size");
	}
	if (state.theta_mass_kg_k_m2.size() != scalar_count) {
		throw std::invalid_argument("Dry-transport theta-mass array has wrong size");
	}
	if (state.edge_normal_mps.size() != edge_count) {
		throw std::invalid_argument("Dry-transport edge-wind array has wrong size");
	}
}

bool VoronoiDryTransport::validate_finite_positive(const State &state) const {
	for (double mass : state.layer_mass_kg_m2) {
		if (!(mass > 0.0) || !std::isfinite(mass)) return false;
	}
	for (double theta_mass : state.theta_mass_kg_k_m2) {
		if (!(theta_mass > 0.0) || !std::isfinite(theta_mass)) return false;
	}
	for (double u : state.edge_normal_mps) {
		if (!std::isfinite(u)) return false;
	}
	return true;
}

VoronoiDryTransport::State VoronoiDryTransport::make_isothermal_reference(
		double surface_pressure_pa, double temperature_k) const {
	const auto primitive = hydrostatic_.make_isothermal_reference(surface_pressure_pa, temperature_k);
	const auto diagnostics = hydrostatic_.diagnose(primitive);

	State state;
	state.layer_mass_kg_m2 = diagnostics.layer_mass_kg_m2;
	state.theta_mass_kg_k_m2.resize(state.layer_mass_kg_m2.size());
	state.edge_normal_mps = primitive.edge_normal_mps;
	for (size_t i = 0; i < state.layer_mass_kg_m2.size(); ++i) {
		state.theta_mass_kg_k_m2[i] = state.layer_mass_kg_m2[i]
			* primitive.potential_temperature_k[i];
	}
	return state;
}

double VoronoiDryTransport::potential_temperature(const State &state,
		int level, int cell) const {
	validate_shape(state);
	if (level < 0 || level >= LEVELS || cell < 0 || cell >= grid_->cell_count()) {
		throw std::out_of_range("Dry-transport potential-temperature index is out of range");
	}
	const int i = scalar_index(level, cell);
	const double mass = state.layer_mass_kg_m2[static_cast<size_t>(i)];
	if (!(mass > 0.0)) throw std::runtime_error("Dry-transport layer mass is non-positive");
	return state.theta_mass_kg_k_m2[static_cast<size_t>(i)] / mass;
}

double VoronoiDryTransport::total_dry_mass_kg(const State &state) const {
	validate_shape(state);
	long double total = 0.0L;
	for (int k = 0; k < LEVELS; ++k) {
		for (int c = 0; c < grid_->cell_count(); ++c) {
			total += static_cast<long double>(state.layer_mass_kg_m2[static_cast<size_t>(scalar_index(k, c))])
				* static_cast<long double>(grid_->cell(c).area_m2);
		}
	}
	return static_cast<double>(total);
}

double VoronoiDryTransport::total_theta_mass_kg_k(const State &state) const {
	validate_shape(state);
	long double total = 0.0L;
	for (int k = 0; k < LEVELS; ++k) {
		for (int c = 0; c < grid_->cell_count(); ++c) {
			total += static_cast<long double>(state.theta_mass_kg_k_m2[static_cast<size_t>(scalar_index(k, c))])
				* static_cast<long double>(grid_->cell(c).area_m2);
		}
	}
	return static_cast<double>(total);
}

double VoronoiDryTransport::max_courant(const State &state, double dt_s) const {
	validate_shape(state);
	if (!(dt_s >= 0.0) || !std::isfinite(dt_s)) {
		throw std::invalid_argument("Dry-transport CFL timestep must be finite and non-negative");
	}
	if (!validate_finite_positive(state)) {
		throw std::runtime_error("Dry-transport state is invalid while computing CFL");
	}

	std::vector<double> outward_rate(static_cast<size_t>(LEVELS)
		* static_cast<size_t>(grid_->cell_count()), 0.0);
	for (int k = 0; k < LEVELS; ++k) {
		for (int e = 0; e < grid_->edge_count(); ++e) {
			const auto &edge = grid_->edge(e);
			const double u = state.edge_normal_mps[static_cast<size_t>(edge_index(k, e))];
			if (u > 0.0) {
				outward_rate[static_cast<size_t>(scalar_index(k, edge.cell_a))]
					+= u * edge.edge_length_m / grid_->cell(edge.cell_a).area_m2;
			} else if (u < 0.0) {
				outward_rate[static_cast<size_t>(scalar_index(k, edge.cell_b))]
					+= (-u) * edge.edge_length_m / grid_->cell(edge.cell_b).area_m2;
			}
		}
	}

	double maximum = 0.0;
	for (double rate : outward_rate) maximum = std::max(maximum, rate * dt_s);
	return maximum;
}

double VoronoiDryTransport::stable_dt(const State &state, double target_cfl,
		double maximum_dt_s) const {
	if (!(target_cfl > 0.0) || !(target_cfl < 1.0) || !std::isfinite(target_cfl)) {
		throw std::invalid_argument("Dry-transport target CFL must be finite and in (0,1)");
	}
	if (!(maximum_dt_s > 0.0) || !std::isfinite(maximum_dt_s)) {
		throw std::invalid_argument("Dry-transport maximum timestep must be finite and positive");
	}
	const double unit_courant = max_courant(state, 1.0);
	if (!(unit_courant > 0.0)) return maximum_dt_s;
	return std::min(maximum_dt_s, target_cfl / unit_courant);
}

VoronoiDryTransport::Tendencies VoronoiDryTransport::compute_tendencies(
		const State &state) const {
	Tendencies tendency;
	const size_t scalar_count = static_cast<size_t>(LEVELS) * static_cast<size_t>(grid_->cell_count());
	tendency.mass_dt.assign(scalar_count, 0.0);
	tendency.theta_mass_dt.assign(scalar_count, 0.0);

	for (int k = 0; k < LEVELS; ++k) {
		for (int e = 0; e < grid_->edge_count(); ++e) {
			const auto &edge = grid_->edge(e);
			const int ia = scalar_index(k, edge.cell_a);
			const int ib = scalar_index(k, edge.cell_b);
			const double u = state.edge_normal_mps[static_cast<size_t>(edge_index(k, e))];
			if (u == 0.0) continue;

			const int donor = u > 0.0 ? ia : ib;
			const double donor_mass = state.layer_mass_kg_m2[static_cast<size_t>(donor)];
			const double donor_theta = state.theta_mass_kg_k_m2[static_cast<size_t>(donor)]
				/ donor_mass;
			// Signed a -> b mass transport [kg/s]. The same flux is consumed with
			// opposite sign by both adjacent cells; theta uses the identical donor.
			const double mass_flux = u * edge.edge_length_m * donor_mass;
			const double theta_flux = mass_flux * donor_theta;
			const double inv_area_a = 1.0 / grid_->cell(edge.cell_a).area_m2;
			const double inv_area_b = 1.0 / grid_->cell(edge.cell_b).area_m2;

			tendency.mass_dt[static_cast<size_t>(ia)] -= mass_flux * inv_area_a;
			tendency.mass_dt[static_cast<size_t>(ib)] += mass_flux * inv_area_b;
			tendency.theta_mass_dt[static_cast<size_t>(ia)] -= theta_flux * inv_area_a;
			tendency.theta_mass_dt[static_cast<size_t>(ib)] += theta_flux * inv_area_b;
		}
	}
	return tendency;
}

bool VoronoiDryTransport::euler_stage(const State &input, State &output,
		double dt_s) const {
	const Tendencies tendency = compute_tendencies(input);
	output = input;
	for (size_t i = 0; i < output.layer_mass_kg_m2.size(); ++i) {
		output.layer_mass_kg_m2[i] += dt_s * tendency.mass_dt[i];
		output.theta_mass_kg_k_m2[i] += dt_s * tendency.theta_mass_dt[i];
	}
	return validate_finite_positive(output);
}

bool VoronoiDryTransport::ssprk3_attempt(const State &initial, State &candidate,
		double dt_s) const {
	State stage1;
	if (!euler_stage(initial, stage1, dt_s)) return false;

	State stage1_euler;
	if (!euler_stage(stage1, stage1_euler, dt_s)) return false;

	State stage2 = initial;
	for (size_t i = 0; i < stage2.layer_mass_kg_m2.size(); ++i) {
		stage2.layer_mass_kg_m2[i] = 0.75 * initial.layer_mass_kg_m2[i]
			+ 0.25 * stage1_euler.layer_mass_kg_m2[i];
		stage2.theta_mass_kg_k_m2[i] = 0.75 * initial.theta_mass_kg_k_m2[i]
			+ 0.25 * stage1_euler.theta_mass_kg_k_m2[i];
	}
	if (!validate_finite_positive(stage2)) return false;

	State stage2_euler;
	if (!euler_stage(stage2, stage2_euler, dt_s)) return false;

	candidate = initial;
	for (size_t i = 0; i < candidate.layer_mass_kg_m2.size(); ++i) {
		candidate.layer_mass_kg_m2[i] = (1.0 / 3.0) * initial.layer_mass_kg_m2[i]
			+ (2.0 / 3.0) * stage2_euler.layer_mass_kg_m2[i];
		candidate.theta_mass_kg_k_m2[i] = (1.0 / 3.0) * initial.theta_mass_kg_k_m2[i]
			+ (2.0 / 3.0) * stage2_euler.theta_mass_kg_k_m2[i];
	}
	return validate_finite_positive(candidate);
}

VoronoiDryTransport::StepDiagnostics VoronoiDryTransport::step(State &state,
		double requested_dt_s, double target_cfl, int max_retries) const {
	validate_shape(state);
	if (!validate_finite_positive(state)) {
		throw std::runtime_error("Dry-transport step received an invalid state");
	}
	if (!(requested_dt_s > 0.0) || !std::isfinite(requested_dt_s)) {
		throw std::invalid_argument("Dry-transport requested timestep must be finite and positive");
	}
	if (!(target_cfl > 0.0) || !(target_cfl < 1.0) || !std::isfinite(target_cfl)) {
		throw std::invalid_argument("Dry-transport target CFL must be finite and in (0,1)");
	}
	if (max_retries < 0) {
		throw std::invalid_argument("Dry-transport retry count cannot be negative");
	}

	StepDiagnostics diagnostics;
	diagnostics.requested_dt_s = requested_dt_s;
	diagnostics.dry_mass_before_kg = total_dry_mass_kg(state);
	diagnostics.theta_mass_before_kg_k = total_theta_mass_kg_k(state);

	bool any_motion = false;
	for (double u : state.edge_normal_mps) {
		if (u != 0.0) {
			any_motion = true;
			break;
		}
	}
	if (!any_motion) {
		// Preserve hydrostatic rest bit-for-bit. Running an algebraically zero
		// RHS through SSPRK3 can still perturb the last bit through convex
		// recombination, which is undesirable for the model's exact no-source state.
		diagnostics.accepted_dt_s = requested_dt_s;
		diagnostics.max_courant = 0.0;
		diagnostics.dry_mass_after_kg = diagnostics.dry_mass_before_kg;
		diagnostics.theta_mass_after_kg_k = diagnostics.theta_mass_before_kg_k;
		diagnostics.relative_dry_mass_error = 0.0;
		diagnostics.relative_theta_mass_error = 0.0;
		diagnostics.min_layer_mass_kg_m2 = std::numeric_limits<double>::infinity();
		diagnostics.min_potential_temperature_k = std::numeric_limits<double>::infinity();
		for (size_t i = 0; i < state.layer_mass_kg_m2.size(); ++i) {
			const double mass = state.layer_mass_kg_m2[i];
			diagnostics.min_layer_mass_kg_m2 = std::min(diagnostics.min_layer_mass_kg_m2, mass);
			diagnostics.min_potential_temperature_k = std::min(
				diagnostics.min_potential_temperature_k, state.theta_mass_kg_k_m2[i] / mass);
		}
		return diagnostics;
	}

	const State original = state;
	double attempt_dt = stable_dt(state, target_cfl, requested_dt_s);
	for (int attempt = 0; attempt <= max_retries; ++attempt) {
		State candidate;
		if (ssprk3_attempt(original, candidate, attempt_dt)) {
			const double mass_after = total_dry_mass_kg(candidate);
			const double theta_after = total_theta_mass_kg_k(candidate);
			const double mass_error = std::abs(mass_after - diagnostics.dry_mass_before_kg)
				/ std::max(std::abs(diagnostics.dry_mass_before_kg), 1.0);
			const double theta_error = std::abs(theta_after - diagnostics.theta_mass_before_kg_k)
				/ std::max(std::abs(diagnostics.theta_mass_before_kg_k), 1.0);
			if (std::isfinite(mass_error) && std::isfinite(theta_error)
					&& mass_error <= CONSERVATION_REJECT_TOL
					&& theta_error <= CONSERVATION_REJECT_TOL) {
				state = std::move(candidate);
				diagnostics.accepted_dt_s = attempt_dt;
				diagnostics.max_courant = max_courant(original, attempt_dt);
				diagnostics.dry_mass_after_kg = mass_after;
				diagnostics.theta_mass_after_kg_k = theta_after;
				diagnostics.relative_dry_mass_error = mass_error;
				diagnostics.relative_theta_mass_error = theta_error;
				break;
			}
		}

		++diagnostics.rejected_steps;
		attempt_dt *= 0.5;
		if (!(attempt_dt > std::numeric_limits<double>::min())) break;
	}

	if (diagnostics.accepted_dt_s == 0.0) {
		state = original;
		diagnostics.dry_mass_after_kg = diagnostics.dry_mass_before_kg;
		diagnostics.theta_mass_after_kg_k = diagnostics.theta_mass_before_kg_k;
		diagnostics.relative_dry_mass_error = 0.0;
		diagnostics.relative_theta_mass_error = 0.0;
	}

	diagnostics.min_layer_mass_kg_m2 = std::numeric_limits<double>::infinity();
	diagnostics.min_potential_temperature_k = std::numeric_limits<double>::infinity();
	diagnostics.max_speed_mps = 0.0;
	for (size_t i = 0; i < state.layer_mass_kg_m2.size(); ++i) {
		const double mass = state.layer_mass_kg_m2[i];
		diagnostics.min_layer_mass_kg_m2 = std::min(diagnostics.min_layer_mass_kg_m2, mass);
		diagnostics.min_potential_temperature_k = std::min(
			diagnostics.min_potential_temperature_k, state.theta_mass_kg_k_m2[i] / mass);
	}
	for (double u : state.edge_normal_mps) {
		diagnostics.max_speed_mps = std::max(diagnostics.max_speed_mps, std::abs(u));
	}
	return diagnostics;
}

} // namespace asterra::weather

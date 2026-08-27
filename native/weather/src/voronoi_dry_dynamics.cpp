#include "voronoi_dry_dynamics.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace asterra::weather {

namespace {
constexpr double GAMMA_DRY = 1.4;
constexpr double CONSERVATION_REJECT_TOL = 1.0e-10;
}

VoronoiDryDynamics::VoronoiDryDynamics(const GeodesicVoronoiGrid &grid,
		double gravity_mps2, double scale_height_m, double top_pressure_pa,
		double rotation_rate_rad_s, Vec3d rotation_axis)
	: grid_(&grid),
	  transport_(grid, gravity_mps2, scale_height_m, top_pressure_pa),
	  rotation_rate_rad_s_(rotation_rate_rad_s) {
	if (!std::isfinite(rotation_rate_rad_s_)) {
		throw std::invalid_argument("Dry dynamics rotation rate must be finite");
	}
	const double axis2 = dot(rotation_axis, rotation_axis);
	if (!(axis2 > 1.0e-24) || !std::isfinite(axis2)) {
		throw std::invalid_argument("Dry dynamics rotation axis must be finite and non-zero");
	}
	rotation_axis_ = rotation_axis / std::sqrt(axis2);
}

void VoronoiDryDynamics::validate_shape(const State &state) const {
	const size_t scalars = static_cast<size_t>(LEVELS) * grid_->cell_count();
	const size_t edges = static_cast<size_t>(LEVELS) * grid_->edge_count();
	if (state.layer_mass_kg_m2.size() != scalars
			|| state.theta_mass_kg_k_m2.size() != scalars
			|| state.edge_normal_mps.size() != edges) {
		throw std::invalid_argument("Dry dynamics state arrays have wrong size");
	}
}

bool VoronoiDryDynamics::validate_finite_positive(const State &state) const {
	for (size_t i = 0; i < state.layer_mass_kg_m2.size(); ++i) {
		const double mass = state.layer_mass_kg_m2[i];
		const double theta_mass = state.theta_mass_kg_k_m2[i];
		if (!(mass > 0.0) || !(theta_mass > 0.0)
				|| !std::isfinite(mass) || !std::isfinite(theta_mass)) return false;
		const double theta = theta_mass / mass;
		if (!(theta > 100.0) || !std::isfinite(theta)) return false;
	}
	for (double u : state.edge_normal_mps) {
		if (!std::isfinite(u)) return false;
	}
	return true;
}

std::vector<double> VoronoiDryDynamics::reconstruct_vertex_relative_vorticity(
		const State &state, int level) const {
	validate_shape(state);
	if (level < 0 || level >= LEVELS) {
		throw std::out_of_range("Dry dynamics vorticity level is out of range");
	}
	std::vector<double> zeta(static_cast<size_t>(grid_->vertex_count()), 0.0);
	for (int v = 0; v < grid_->vertex_count(); ++v) {
		const auto &vertex = grid_->vertex(v);
		long double circulation = 0.0L;
		for (int q = 0; q < 3; ++q) {
			const int e = vertex.edges[q];
			const auto &edge = grid_->edge(e);
			double sign = 0.0;
			if (edge.vertex_b == v) sign = 1.0;
			else if (edge.vertex_a == v) sign = -1.0;
			else throw std::runtime_error("Dry dynamics dual vertex/edge connectivity is inconsistent");
			circulation += static_cast<long double>(sign)
				* static_cast<long double>(edge.center_distance_m)
				* static_cast<long double>(state.edge_normal_mps[
					static_cast<size_t>(edge_index(level, e))]);
		}
		zeta[static_cast<size_t>(v)] = static_cast<double>(
			circulation / static_cast<long double>(vertex.dual_area_m2));
	}
	return zeta;
}

std::vector<double> VoronoiDryDynamics::cell_kinetic_energy(const State &state,
		int level) const {
	std::vector<double> kinetic(static_cast<size_t>(grid_->cell_count()), 0.0);
	for (int c = 0; c < grid_->cell_count(); ++c) {
		const auto &cell = grid_->cell(c);
		long double sum = 0.0L;
		for (int e : cell.edges) {
			const double u = state.edge_normal_mps[static_cast<size_t>(edge_index(level, e))];
			sum += static_cast<long double>(grid_->edge(e).edge_area_m2)
				* static_cast<long double>(u * u);
		}
		kinetic[static_cast<size_t>(c)] = static_cast<double>(
			sum / (4.0L * static_cast<long double>(cell.area_m2)));
	}
	return kinetic;
}

double VoronoiDryDynamics::max_courant(const State &state, double dt_s) const {
	validate_shape(state);
	if (!validate_finite_positive(state)) {
		throw std::runtime_error("Dry dynamics CFL received invalid state");
	}
	if (!(dt_s >= 0.0) || !std::isfinite(dt_s)) {
		throw std::invalid_argument("Dry dynamics CFL timestep must be finite and non-negative");
	}

	double maximum = transport_.max_courant(state, dt_s);
	const auto hydro = transport_.diagnose_hydrostatic(state);
	for (int k = 0; k < LEVELS; ++k) {
		for (int e = 0; e < grid_->edge_count(); ++e) {
			const auto &edge = grid_->edge(e);
			const int ia = scalar_index(k, edge.cell_a);
			const int ib = scalar_index(k, edge.cell_b);
			const double t_edge = 0.5 * (hydro.temperature_k[static_cast<size_t>(ia)]
				+ hydro.temperature_k[static_cast<size_t>(ib)]);
			const double pressure_wave_speed = std::sqrt(
				GAMMA_DRY * VoronoiDryHydrostatic::RD * t_edge);
			const double u = std::abs(state.edge_normal_mps[static_cast<size_t>(edge_index(k, e))]);
			maximum = std::max(maximum,
				dt_s * (u + pressure_wave_speed) / edge.center_distance_m);
		}
	}
	maximum = std::max(maximum, dt_s * 2.0 * std::abs(rotation_rate_rad_s_));
	return maximum;
}

double VoronoiDryDynamics::stable_dt(const State &state, double target_cfl,
		double maximum_dt_s) const {
	if (!(target_cfl > 0.0) || target_cfl > 0.5 || !std::isfinite(target_cfl)) {
		throw std::invalid_argument("Dry dynamics target CFL must be finite and in (0,0.5]");
	}
	if (!(maximum_dt_s > 0.0) || !std::isfinite(maximum_dt_s)) {
		throw std::invalid_argument("Dry dynamics maximum timestep must be finite and positive");
	}
	const double unit = max_courant(state, 1.0);
	if (!(unit > 0.0) || !std::isfinite(unit)) {
		throw std::runtime_error("Dry dynamics state has invalid characteristic speed");
	}
	return std::min(maximum_dt_s, target_cfl / unit);
}

VoronoiDryDynamics::Tendencies VoronoiDryDynamics::compute_tendencies(
		const State &state) const {
	validate_shape(state);
	if (!validate_finite_positive(state)) {
		throw std::runtime_error("Dry dynamics tendency received invalid state");
	}

	const size_t scalar_count = static_cast<size_t>(LEVELS) * grid_->cell_count();
	const size_t edge_count = static_cast<size_t>(LEVELS) * grid_->edge_count();
	Tendencies t;
	t.mass_dt.assign(scalar_count, 0.0);
	t.theta_mass_dt.assign(scalar_count, 0.0);
	t.edge_velocity_dt.assign(edge_count, 0.0);
	t.normal_mass_flux.assign(edge_count, 0.0);

	// Exactly one donor mass flux exists per physical edge and level. The same
	// flux transports dry mass and mass-weighted theta.
	for (int k = 0; k < LEVELS; ++k) {
		for (int e = 0; e < grid_->edge_count(); ++e) {
			const auto &edge = grid_->edge(e);
			const int ia = scalar_index(k, edge.cell_a);
			const int ib = scalar_index(k, edge.cell_b);
			const int ei = edge_index(k, e);
			const double u = state.edge_normal_mps[static_cast<size_t>(ei)];
			if (u == 0.0) continue;
			const int donor = u > 0.0 ? ia : ib;
			const double donor_mass = state.layer_mass_kg_m2[static_cast<size_t>(donor)];
			const double donor_theta = state.theta_mass_kg_k_m2[static_cast<size_t>(donor)]
				/ donor_mass;
			const double normal_mass_flux = donor_mass * u;
			const double mass_rate = edge.edge_length_m * normal_mass_flux;
			const double theta_rate = mass_rate * donor_theta;
			t.normal_mass_flux[static_cast<size_t>(ei)] = normal_mass_flux;
			t.mass_dt[static_cast<size_t>(ia)] -= mass_rate / grid_->cell(edge.cell_a).area_m2;
			t.mass_dt[static_cast<size_t>(ib)] += mass_rate / grid_->cell(edge.cell_b).area_m2;
			t.theta_mass_dt[static_cast<size_t>(ia)] -= theta_rate / grid_->cell(edge.cell_a).area_m2;
			t.theta_mass_dt[static_cast<size_t>(ib)] += theta_rate / grid_->cell(edge.cell_b).area_m2;
		}
	}

	const auto hydro = transport_.diagnose_hydrostatic(state);
	const auto pressure_accel = transport_.pressure_gradient_acceleration(state, hydro);
	for (double a : pressure_accel) {
		t.max_pressure_acceleration_mps2 = std::max(
			t.max_pressure_acceleration_mps2, std::abs(a));
	}

	for (int k = 0; k < LEVELS; ++k) {
		const std::vector<double> kinetic = cell_kinetic_energy(state, k);
		const std::vector<double> zeta = reconstruct_vertex_relative_vorticity(state, k);
		std::vector<double> q_vertex(static_cast<size_t>(grid_->vertex_count()), 0.0);
		for (int v = 0; v < grid_->vertex_count(); ++v) {
			const auto &vertex = grid_->vertex(v);
			long double weighted_mass = 0.0L;
			for (int j = 0; j < 3; ++j) {
				weighted_mass += static_cast<long double>(vertex.kite_area_m2[j])
					* static_cast<long double>(state.layer_mass_kg_m2[
						static_cast<size_t>(scalar_index(k, vertex.cells[j]))]);
			}
			const double mass_vertex = static_cast<double>(
				weighted_mass / static_cast<long double>(vertex.dual_area_m2));
			if (!(mass_vertex > 0.0) || !std::isfinite(mass_vertex)) {
				throw std::runtime_error("Dry dynamics reconstructed invalid vertex mass");
			}
			const double coriolis = 2.0 * rotation_rate_rad_s_ * dot(rotation_axis_, vertex.center);
			q_vertex[static_cast<size_t>(v)] = (zeta[static_cast<size_t>(v)] + coriolis)
				/ mass_vertex;
		}

		std::vector<double> q_edge(static_cast<size_t>(grid_->edge_count()), 0.0);
		for (int e = 0; e < grid_->edge_count(); ++e) {
			const auto &edge = grid_->edge(e);
			q_edge[static_cast<size_t>(e)] = 0.5 * (
				q_vertex[static_cast<size_t>(edge.vertex_a)]
				+ q_vertex[static_cast<size_t>(edge.vertex_b)]);
			const double grad_k = (kinetic[static_cast<size_t>(edge.cell_b)]
				- kinetic[static_cast<size_t>(edge.cell_a)]) / edge.center_distance_m;
			t.edge_velocity_dt[static_cast<size_t>(edge_index(k, e))]
				= pressure_accel[static_cast<size_t>(edge_index(k, e))] - grad_k;
		}

		// TRiSK absolute-vorticity flux. q has units (1/s)/(kg/m2), and
		// normal_mass_flux has kg/(m s), giving the required m/s2 acceleration.
		for (int e = 0; e < grid_->edge_count(); ++e) {
			const auto &target = grid_->edge(e);
			if (target.reconstruction_edges.size() != target.reconstruction_weights.size()) {
				throw std::runtime_error("Dry dynamics TRiSK reconstruction size mismatch");
			}
			double q_flux_perp = 0.0;
			for (size_t j = 0; j < target.reconstruction_edges.size(); ++j) {
				const int source = target.reconstruction_edges[j];
				const double q_pair = 0.5 * (q_edge[static_cast<size_t>(e)]
					+ q_edge[static_cast<size_t>(source)]);
				q_flux_perp += target.reconstruction_weights[j] * q_pair
					* t.normal_mass_flux[static_cast<size_t>(edge_index(k, source))];
			}
			t.edge_velocity_dt[static_cast<size_t>(edge_index(k, e))] += q_flux_perp;
		}
	}
	return t;
}

bool VoronoiDryDynamics::euler_stage(const State &input, State &output,
		double dt_s, double &max_pressure_acceleration_mps2) const {
	try {
		const Tendencies t = compute_tendencies(input);
		max_pressure_acceleration_mps2 = std::max(max_pressure_acceleration_mps2,
			t.max_pressure_acceleration_mps2);
		output = input;
		for (size_t i = 0; i < output.layer_mass_kg_m2.size(); ++i) {
			output.layer_mass_kg_m2[i] += dt_s * t.mass_dt[i];
			output.theta_mass_kg_k_m2[i] += dt_s * t.theta_mass_dt[i];
		}
		for (size_t i = 0; i < output.edge_normal_mps.size(); ++i) {
			output.edge_normal_mps[i] += dt_s * t.edge_velocity_dt[i];
		}
		return validate_finite_positive(output);
	} catch (const std::exception &) {
		return false;
	}
}

bool VoronoiDryDynamics::ssprk3_attempt(const State &initial, State &candidate,
		double dt_s, double &max_pressure_acceleration_mps2) const {
	State s1;
	if (!euler_stage(initial, s1, dt_s, max_pressure_acceleration_mps2)) return false;
	State euler2;
	if (!euler_stage(s1, euler2, dt_s, max_pressure_acceleration_mps2)) return false;

	State s2 = initial;
	for (size_t i = 0; i < s2.layer_mass_kg_m2.size(); ++i) {
		s2.layer_mass_kg_m2[i] = 0.75 * initial.layer_mass_kg_m2[i]
			+ 0.25 * euler2.layer_mass_kg_m2[i];
		s2.theta_mass_kg_k_m2[i] = 0.75 * initial.theta_mass_kg_k_m2[i]
			+ 0.25 * euler2.theta_mass_kg_k_m2[i];
	}
	for (size_t i = 0; i < s2.edge_normal_mps.size(); ++i) {
		s2.edge_normal_mps[i] = 0.75 * initial.edge_normal_mps[i]
			+ 0.25 * euler2.edge_normal_mps[i];
	}
	if (!validate_finite_positive(s2)) return false;

	State euler3;
	if (!euler_stage(s2, euler3, dt_s, max_pressure_acceleration_mps2)) return false;
	candidate = initial;
	for (size_t i = 0; i < candidate.layer_mass_kg_m2.size(); ++i) {
		candidate.layer_mass_kg_m2[i] = (1.0 / 3.0) * initial.layer_mass_kg_m2[i]
			+ (2.0 / 3.0) * euler3.layer_mass_kg_m2[i];
		candidate.theta_mass_kg_k_m2[i] = (1.0 / 3.0) * initial.theta_mass_kg_k_m2[i]
			+ (2.0 / 3.0) * euler3.theta_mass_kg_k_m2[i];
	}
	for (size_t i = 0; i < candidate.edge_normal_mps.size(); ++i) {
		candidate.edge_normal_mps[i] = (1.0 / 3.0) * initial.edge_normal_mps[i]
			+ (2.0 / 3.0) * euler3.edge_normal_mps[i];
	}
	return validate_finite_positive(candidate);
}

VoronoiDryDynamics::StepDiagnostics VoronoiDryDynamics::step(State &state,
		double requested_dt_s, double target_cfl, int max_retries) const {
	validate_shape(state);
	if (!validate_finite_positive(state)) {
		throw std::runtime_error("Dry dynamics step received invalid state");
	}
	if (!(requested_dt_s > 0.0) || !std::isfinite(requested_dt_s)) {
		throw std::invalid_argument("Dry dynamics requested timestep must be finite and positive");
	}
	if (!(target_cfl > 0.0) || target_cfl > 0.5 || !std::isfinite(target_cfl)) {
		throw std::invalid_argument("Dry dynamics target CFL must be finite and in (0,0.5]");
	}
	if (max_retries < 0) throw std::invalid_argument("Dry dynamics retry count cannot be negative");

	StepDiagnostics diagnostics;
	diagnostics.requested_dt_s = requested_dt_s;
	diagnostics.dry_mass_before_kg = total_dry_mass_kg(state);
	diagnostics.theta_mass_before_kg_k = total_theta_mass_kg_k(state);

	bool any_motion = false;
	for (double u : state.edge_normal_mps) if (u != 0.0) { any_motion = true; break; }
	if (!any_motion) {
		const auto hydro = transport_.diagnose_hydrostatic(state);
		const auto pressure = transport_.pressure_gradient_acceleration(state, hydro);
		bool any_force = false;
		for (double a : pressure) {
			diagnostics.max_pressure_acceleration_mps2 = std::max(
				diagnostics.max_pressure_acceleration_mps2, std::abs(a));
			if (a != 0.0) any_force = true;
		}
		if (!any_force) {
			diagnostics.accepted_dt_s = requested_dt_s;
			diagnostics.dry_mass_after_kg = diagnostics.dry_mass_before_kg;
			diagnostics.theta_mass_after_kg_k = diagnostics.theta_mass_before_kg_k;
			diagnostics.min_layer_mass_kg_m2 = std::numeric_limits<double>::infinity();
			diagnostics.min_potential_temperature_k = std::numeric_limits<double>::infinity();
			for (size_t i = 0; i < state.layer_mass_kg_m2.size(); ++i) {
				const double mass = state.layer_mass_kg_m2[i];
				diagnostics.min_layer_mass_kg_m2 = std::min(
					diagnostics.min_layer_mass_kg_m2, mass);
				diagnostics.min_potential_temperature_k = std::min(
					diagnostics.min_potential_temperature_k, state.theta_mass_kg_k_m2[i] / mass);
			}
			return diagnostics;
		}
	}

	const State original = state;
	double attempt_dt = stable_dt(original, target_cfl, requested_dt_s);
	for (int attempt = 0; attempt <= max_retries; ++attempt) {
		State candidate;
		double attempt_max_pressure = 0.0;
		if (ssprk3_attempt(original, candidate, attempt_dt, attempt_max_pressure)) {
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
				diagnostics.max_pressure_acceleration_mps2 = attempt_max_pressure;
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

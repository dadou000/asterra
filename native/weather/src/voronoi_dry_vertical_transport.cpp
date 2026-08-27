#include "voronoi_dry_vertical_transport.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace asterra::weather {

namespace {
constexpr double CONSERVATION_REJECT_TOL = 1.0e-11;
}

VoronoiDryVerticalTransport::VoronoiDryVerticalTransport(
		const GeodesicVoronoiGrid &grid, double gravity_mps2,
		double scale_height_m, double top_pressure_pa)
	: grid_(&grid), horizontal_(grid, gravity_mps2, scale_height_m, top_pressure_pa) {
	if (grid.cell_count() <= 0) {
		throw std::invalid_argument("Dry vertical transport requires a built geodesic grid");
	}
}

void VoronoiDryVerticalTransport::validate_state(const State &state) const {
	const size_t scalar_count = static_cast<size_t>(LEVELS) * grid_->cell_count();
	const size_t edge_count = static_cast<size_t>(LEVELS) * grid_->edge_count();
	if (state.layer_mass_kg_m2.size() != scalar_count
			|| state.theta_mass_kg_k_m2.size() != scalar_count
			|| state.edge_normal_mps.size() != edge_count) {
		throw std::invalid_argument("Dry vertical transport state arrays have wrong size");
	}
}

void VoronoiDryVerticalTransport::validate_flux(
		const std::vector<double> &interface_mass_flux) const {
	const size_t expected = static_cast<size_t>(INTERFACES) * grid_->cell_count();
	if (interface_mass_flux.size() != expected) {
		throw std::invalid_argument("Dry vertical interface-mass-flux array has wrong size");
	}
	for (double f : interface_mass_flux) {
		if (!std::isfinite(f)) {
			throw std::invalid_argument("Dry vertical interface mass flux must be finite");
		}
	}
}

bool VoronoiDryVerticalTransport::finite_positive(const State &state) const {
	for (size_t i = 0; i < state.layer_mass_kg_m2.size(); ++i) {
		const double mass = state.layer_mass_kg_m2[i];
		const double theta_mass = state.theta_mass_kg_k_m2[i];
		if (!(mass > 0.0) || !(theta_mass > 0.0)
				|| !std::isfinite(mass) || !std::isfinite(theta_mass)) return false;
		const double theta = theta_mass / mass;
		if (!(theta > 100.0) || !std::isfinite(theta)) return false;
	}
	for (double u : state.edge_normal_mps) if (!std::isfinite(u)) return false;
	return true;
}

double VoronoiDryVerticalTransport::max_courant(const State &state,
		const std::vector<double> &interface_mass_flux, double dt_s) const {
	validate_state(state);
	validate_flux(interface_mass_flux);
	if (!finite_positive(state)) {
		throw std::runtime_error("Dry vertical CFL received an invalid state");
	}
	if (!(dt_s >= 0.0) || !std::isfinite(dt_s)) {
		throw std::invalid_argument("Dry vertical CFL timestep must be finite and non-negative");
	}

	std::vector<double> outgoing_rate(
		static_cast<size_t>(LEVELS) * grid_->cell_count(), 0.0);
	for (int j = 0; j < INTERFACES; ++j) {
		for (int c = 0; c < grid_->cell_count(); ++c) {
			const double flux = interface_mass_flux[static_cast<size_t>(interface_index(j, c))];
			if (flux > 0.0) {
				outgoing_rate[static_cast<size_t>(scalar_index(j, c))] += flux;
			} else if (flux < 0.0) {
				outgoing_rate[static_cast<size_t>(scalar_index(j + 1, c))] += -flux;
			}
		}
	}

	double maximum = 0.0;
	for (int k = 0; k < LEVELS; ++k) {
		for (int c = 0; c < grid_->cell_count(); ++c) {
			const int i = scalar_index(k, c);
			const double rate = outgoing_rate[static_cast<size_t>(i)]
				/ state.layer_mass_kg_m2[static_cast<size_t>(i)];
			maximum = std::max(maximum, dt_s * rate);
		}
	}
	return maximum;
}

double VoronoiDryVerticalTransport::stable_dt(const State &state,
		const std::vector<double> &interface_mass_flux,
		double target_cfl, double maximum_dt_s) const {
	if (!(target_cfl > 0.0) || !(target_cfl < 1.0) || !std::isfinite(target_cfl)) {
		throw std::invalid_argument("Dry vertical target CFL must be finite and in (0,1)");
	}
	if (!(maximum_dt_s > 0.0) || !std::isfinite(maximum_dt_s)) {
		throw std::invalid_argument("Dry vertical maximum timestep must be finite and positive");
	}
	const double unit = max_courant(state, interface_mass_flux, 1.0);
	if (!(unit > 0.0)) return maximum_dt_s;
	return std::min(maximum_dt_s, target_cfl / unit);
}

VoronoiDryVerticalTransport::Tendencies
VoronoiDryVerticalTransport::compute_tendencies(const State &state,
		const std::vector<double> &interface_mass_flux) const {
	Tendencies t;
	const size_t scalar_count = static_cast<size_t>(LEVELS) * grid_->cell_count();
	t.mass_dt.assign(scalar_count, 0.0);
	t.theta_mass_dt.assign(scalar_count, 0.0);

	for (int j = 0; j < INTERFACES; ++j) {
		for (int c = 0; c < grid_->cell_count(); ++c) {
			const double flux = interface_mass_flux[static_cast<size_t>(interface_index(j, c))];
			if (flux == 0.0) continue;
			const int lower = scalar_index(j, c);
			const int upper = scalar_index(j + 1, c);
			const int donor = flux > 0.0 ? lower : upper;
			const double donor_mass = state.layer_mass_kg_m2[static_cast<size_t>(donor)];
			const double donor_theta = state.theta_mass_kg_k_m2[static_cast<size_t>(donor)]
				/ donor_mass;
			const double theta_flux = flux * donor_theta;

			t.mass_dt[static_cast<size_t>(lower)] -= flux;
			t.mass_dt[static_cast<size_t>(upper)] += flux;
			t.theta_mass_dt[static_cast<size_t>(lower)] -= theta_flux;
			t.theta_mass_dt[static_cast<size_t>(upper)] += theta_flux;
		}
	}
	return t;
}

bool VoronoiDryVerticalTransport::euler_stage(const State &input, State &output,
		const std::vector<double> &interface_mass_flux, double dt_s) const {
	const Tendencies t = compute_tendencies(input, interface_mass_flux);
	output = input;
	for (size_t i = 0; i < output.layer_mass_kg_m2.size(); ++i) {
		output.layer_mass_kg_m2[i] += dt_s * t.mass_dt[i];
		output.theta_mass_kg_k_m2[i] += dt_s * t.theta_mass_dt[i];
	}
	return finite_positive(output);
}

bool VoronoiDryVerticalTransport::ssprk3_attempt(const State &initial,
		State &candidate, const std::vector<double> &interface_mass_flux,
		double dt_s) const {
	State s1;
	if (!euler_stage(initial, s1, interface_mass_flux, dt_s)) return false;
	State e2;
	if (!euler_stage(s1, e2, interface_mass_flux, dt_s)) return false;

	State s2 = initial;
	for (size_t i = 0; i < s2.layer_mass_kg_m2.size(); ++i) {
		s2.layer_mass_kg_m2[i] = 0.75 * initial.layer_mass_kg_m2[i]
			+ 0.25 * e2.layer_mass_kg_m2[i];
		s2.theta_mass_kg_k_m2[i] = 0.75 * initial.theta_mass_kg_k_m2[i]
			+ 0.25 * e2.theta_mass_kg_k_m2[i];
	}
	if (!finite_positive(s2)) return false;

	State e3;
	if (!euler_stage(s2, e3, interface_mass_flux, dt_s)) return false;
	candidate = initial;
	for (size_t i = 0; i < candidate.layer_mass_kg_m2.size(); ++i) {
		candidate.layer_mass_kg_m2[i] = (1.0 / 3.0) * initial.layer_mass_kg_m2[i]
			+ (2.0 / 3.0) * e3.layer_mass_kg_m2[i];
		candidate.theta_mass_kg_k_m2[i] = (1.0 / 3.0) * initial.theta_mass_kg_k_m2[i]
			+ (2.0 / 3.0) * e3.theta_mass_kg_k_m2[i];
	}
	return finite_positive(candidate);
}

VoronoiDryVerticalTransport::StepDiagnostics
VoronoiDryVerticalTransport::step(State &state,
		const std::vector<double> &interface_mass_flux,
		double requested_dt_s, double target_cfl, int max_retries) const {
	validate_state(state);
	validate_flux(interface_mass_flux);
	if (!finite_positive(state)) {
		throw std::runtime_error("Dry vertical step received invalid state");
	}
	if (!(requested_dt_s > 0.0) || !std::isfinite(requested_dt_s)) {
		throw std::invalid_argument("Dry vertical requested timestep must be finite and positive");
	}
	if (!(target_cfl > 0.0) || !(target_cfl < 1.0) || !std::isfinite(target_cfl)) {
		throw std::invalid_argument("Dry vertical target CFL must be finite and in (0,1)");
	}
	if (max_retries < 0) throw std::invalid_argument("Dry vertical retry count cannot be negative");

	StepDiagnostics d;
	d.requested_dt_s = requested_dt_s;
	d.dry_mass_before_kg = horizontal_.total_dry_mass_kg(state);
	d.theta_mass_before_kg_k = horizontal_.total_theta_mass_kg_k(state);

	bool any_flux = false;
	for (double f : interface_mass_flux) if (f != 0.0) { any_flux = true; break; }
	if (!any_flux) {
		d.accepted_dt_s = requested_dt_s;
		d.dry_mass_after_kg = d.dry_mass_before_kg;
		d.theta_mass_after_kg_k = d.theta_mass_before_kg_k;
		d.min_layer_mass_kg_m2 = std::numeric_limits<double>::infinity();
		d.min_potential_temperature_k = std::numeric_limits<double>::infinity();
		for (size_t i = 0; i < state.layer_mass_kg_m2.size(); ++i) {
			const double mass = state.layer_mass_kg_m2[i];
			d.min_layer_mass_kg_m2 = std::min(d.min_layer_mass_kg_m2, mass);
			d.min_potential_temperature_k = std::min(
				d.min_potential_temperature_k, state.theta_mass_kg_k_m2[i] / mass);
		}
		return d;
	}

	const State original = state;
	double attempt_dt = stable_dt(original, interface_mass_flux, target_cfl, requested_dt_s);
	for (int attempt = 0; attempt <= max_retries; ++attempt) {
		State candidate;
		if (ssprk3_attempt(original, candidate, interface_mass_flux, attempt_dt)) {
			const double mass_after = horizontal_.total_dry_mass_kg(candidate);
			const double theta_after = horizontal_.total_theta_mass_kg_k(candidate);
			const double mass_error = std::abs(mass_after - d.dry_mass_before_kg)
				/ std::max(std::abs(d.dry_mass_before_kg), 1.0);
			const double theta_error = std::abs(theta_after - d.theta_mass_before_kg_k)
				/ std::max(std::abs(d.theta_mass_before_kg_k), 1.0);
			if (std::isfinite(mass_error) && std::isfinite(theta_error)
					&& mass_error <= CONSERVATION_REJECT_TOL
					&& theta_error <= CONSERVATION_REJECT_TOL) {
				state = std::move(candidate);
				d.accepted_dt_s = attempt_dt;
				d.max_vertical_courant = max_courant(original, interface_mass_flux, attempt_dt);
				d.dry_mass_after_kg = mass_after;
				d.theta_mass_after_kg_k = theta_after;
				d.relative_dry_mass_error = mass_error;
				d.relative_theta_mass_error = theta_error;
				break;
			}
		}
		++d.rejected_steps;
		attempt_dt *= 0.5;
		if (!(attempt_dt > std::numeric_limits<double>::min())) break;
	}

	if (d.accepted_dt_s == 0.0) {
		state = original;
		d.dry_mass_after_kg = d.dry_mass_before_kg;
		d.theta_mass_after_kg_k = d.theta_mass_before_kg_k;
	}

	d.min_layer_mass_kg_m2 = std::numeric_limits<double>::infinity();
	d.min_potential_temperature_k = std::numeric_limits<double>::infinity();
	for (size_t i = 0; i < state.layer_mass_kg_m2.size(); ++i) {
		const double mass = state.layer_mass_kg_m2[i];
		d.min_layer_mass_kg_m2 = std::min(d.min_layer_mass_kg_m2, mass);
		d.min_potential_temperature_k = std::min(
			d.min_potential_temperature_k, state.theta_mass_kg_k_m2[i] / mass);
	}
	return d;
}

} // namespace asterra::weather

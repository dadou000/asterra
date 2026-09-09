#include "voronoi_dry_vertical_transport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace asterra::weather {

namespace {
constexpr double CONSERVATION_REJECT_TOL = 1.0e-11;
constexpr double REMAP_REJECT_TOL = 2.0e-11;

template <size_t N>
std::array<double, N> remap_extensive_by_mass_overlap(
		const std::array<double, N> &source_mass,
		const std::array<double, N> &source_extensive,
		const std::array<double, N> &target_mass) {
	std::array<double, N> source_lo{};
	std::array<double, N> source_hi{};
	std::array<double, N> target_lo{};
	std::array<double, N> target_hi{};
	std::array<double, N> target_extensive{};

	double cumulative = 0.0;
	for (size_t k = 0; k < N; ++k) {
		source_lo[k] = cumulative;
		cumulative += source_mass[k];
		source_hi[k] = cumulative;
	}
	cumulative = 0.0;
	for (size_t k = 0; k < N; ++k) {
		target_lo[k] = cumulative;
		cumulative += target_mass[k];
		target_hi[k] = cumulative;
	}

	for (size_t t = 0; t < N; ++t) {
		long double q = 0.0L;
		for (size_t s = 0; s < N; ++s) {
			const double overlap = std::max(0.0,
				std::min(target_hi[t], source_hi[s]) - std::max(target_lo[t], source_lo[s]));
			if (overlap == 0.0) continue;
			if (!(source_mass[s] > 0.0)) {
				throw std::runtime_error("Dry coordinate remap encountered non-positive source mass");
			}
			q += static_cast<long double>(overlap)
				* static_cast<long double>(source_extensive[s] / source_mass[s]);
		}
		target_extensive[t] = static_cast<double>(q);
	}
	return target_extensive;
}

double relative_error(double after, double before) {
	return std::abs(after - before) / std::max(std::abs(before), 1.0);
}
} // namespace

VoronoiDryVerticalTransport::VoronoiDryVerticalTransport(
		const GeodesicVoronoiGrid &grid, double gravity_mps2,
		double scale_height_m, double top_pressure_pa)
	: grid_(&grid), horizontal_(grid, gravity_mps2, scale_height_m, top_pressure_pa) {
	if (grid.cell_count() <= 0) {
		throw std::invalid_argument("Dry vertical transport requires a built geodesic grid");
	}

	const State reference = horizontal_.make_isothermal_reference(100000.0, 288.0);
	double total = 0.0;
	for (int k = 0; k < LEVELS; ++k) {
		total += reference.layer_mass_kg_m2[static_cast<size_t>(scalar_index(k, 0))];
	}
	if (!(total > 0.0) || !std::isfinite(total)) {
		throw std::runtime_error("Dry coordinate reference column has invalid total mass");
	}
	double fraction_sum = 0.0;
	for (int k = 0; k < LEVELS - 1; ++k) {
		reference_mass_fraction_[static_cast<size_t>(k)]
			= reference.layer_mass_kg_m2[static_cast<size_t>(scalar_index(k, 0))] / total;
		fraction_sum += reference_mass_fraction_[static_cast<size_t>(k)];
	}
	reference_mass_fraction_[LEVELS - 1] = 1.0 - fraction_sum;
	for (double f : reference_mass_fraction_) {
		if (!(f > 0.0) || !std::isfinite(f)) {
			throw std::runtime_error("Dry coordinate reference mass fractions are invalid");
		}
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
	for (const auto &tracer : state.tracer_mass_kg_m2) {
		if (tracer.size() != scalar_count) {
			throw std::invalid_argument("Dry vertical tracer-mass array has wrong size");
		}
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
	for (const auto &tracer : state.tracer_mass_kg_m2) {
		for (double mass : tracer) {
			if (!(mass >= 0.0) || !std::isfinite(mass)) return false;
		}
	}
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
	t.tracer_mass_dt.resize(state.tracer_mass_kg_m2.size());
	for (auto &field : t.tracer_mass_dt) field.assign(scalar_count, 0.0);

	for (int j = 0; j < INTERFACES; ++j) {
		for (int c = 0; c < grid_->cell_count(); ++c) {
			const double flux = interface_mass_flux[static_cast<size_t>(interface_index(j, c))];
			if (flux == 0.0) continue;
			const int lower = scalar_index(j, c);
			const int upper = scalar_index(j + 1, c);
			const int donor = flux > 0.0 ? lower : upper;
			const double donor_mass = state.layer_mass_kg_m2[static_cast<size_t>(donor)];
			const double donor_theta = state.theta_mass_kg_k_m2[static_cast<size_t>(donor)] / donor_mass;
			const double theta_flux = flux * donor_theta;

			t.mass_dt[static_cast<size_t>(lower)] -= flux;
			t.mass_dt[static_cast<size_t>(upper)] += flux;
			t.theta_mass_dt[static_cast<size_t>(lower)] -= theta_flux;
			t.theta_mass_dt[static_cast<size_t>(upper)] += theta_flux;
			for (size_t tracer = 0; tracer < state.tracer_mass_kg_m2.size(); ++tracer) {
				const double mixing_ratio = state.tracer_mass_kg_m2[tracer][static_cast<size_t>(donor)]
					/ donor_mass;
				const double tracer_flux = flux * mixing_ratio;
				t.tracer_mass_dt[tracer][static_cast<size_t>(lower)] -= tracer_flux;
				t.tracer_mass_dt[tracer][static_cast<size_t>(upper)] += tracer_flux;
			}
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
	for (size_t tracer = 0; tracer < output.tracer_mass_kg_m2.size(); ++tracer) {
		for (size_t i = 0; i < output.tracer_mass_kg_m2[tracer].size(); ++i) {
			output.tracer_mass_kg_m2[tracer][i] += dt_s * t.tracer_mass_dt[tracer][i];
		}
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
	for (size_t tracer = 0; tracer < s2.tracer_mass_kg_m2.size(); ++tracer) {
		for (size_t i = 0; i < s2.tracer_mass_kg_m2[tracer].size(); ++i) {
			s2.tracer_mass_kg_m2[tracer][i] = 0.75 * initial.tracer_mass_kg_m2[tracer][i]
				+ 0.25 * e2.tracer_mass_kg_m2[tracer][i];
		}
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
	for (size_t tracer = 0; tracer < candidate.tracer_mass_kg_m2.size(); ++tracer) {
		for (size_t i = 0; i < candidate.tracer_mass_kg_m2[tracer].size(); ++i) {
			candidate.tracer_mass_kg_m2[tracer][i] = (1.0 / 3.0) * initial.tracer_mass_kg_m2[tracer][i]
				+ (2.0 / 3.0) * e3.tracer_mass_kg_m2[tracer][i];
		}
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
	std::vector<double> tracer_before(state.tracer_mass_kg_m2.size(), 0.0);
	for (size_t tracer = 0; tracer < tracer_before.size(); ++tracer) {
		tracer_before[tracer] = horizontal_.total_tracer_mass_kg(state, static_cast<int>(tracer));
	}

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
			const double mass_error = relative_error(mass_after, d.dry_mass_before_kg);
			const double theta_error = relative_error(theta_after, d.theta_mass_before_kg_k);
			double tracer_error = 0.0;
			for (size_t tracer = 0; tracer < tracer_before.size(); ++tracer) {
				tracer_error = std::max(tracer_error, relative_error(
					horizontal_.total_tracer_mass_kg(candidate, static_cast<int>(tracer)),
					tracer_before[tracer]));
			}
			if (std::isfinite(mass_error) && std::isfinite(theta_error)
					&& std::isfinite(tracer_error)
					&& mass_error <= CONSERVATION_REJECT_TOL
					&& theta_error <= CONSERVATION_REJECT_TOL
					&& tracer_error <= CONSERVATION_REJECT_TOL) {
				state = std::move(candidate);
				d.accepted_dt_s = attempt_dt;
				d.max_vertical_courant = max_courant(original, interface_mass_flux, attempt_dt);
				d.dry_mass_after_kg = mass_after;
				d.theta_mass_after_kg_k = theta_after;
				d.relative_dry_mass_error = mass_error;
				d.relative_theta_mass_error = theta_error;
				d.max_relative_tracer_mass_error = tracer_error;
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

VoronoiDryVerticalTransport::RemapDiagnostics
VoronoiDryVerticalTransport::remap_to_reference_levels(State &state) const {
	validate_state(state);
	if (!finite_positive(state)) {
		throw std::runtime_error("Dry coordinate remap received invalid state");
	}

	const State original = state;
	RemapDiagnostics d;
	const double global_mass_before = horizontal_.total_dry_mass_kg(original);
	const double global_theta_before = horizontal_.total_theta_mass_kg_k(original);
	std::vector<double> global_tracer_before(original.tracer_mass_kg_m2.size(), 0.0);
	for (size_t tracer = 0; tracer < global_tracer_before.size(); ++tracer) {
		global_tracer_before[tracer]
			= horizontal_.total_tracer_mass_kg(original, static_cast<int>(tracer));
	}
	std::vector<double> column_mass_before(static_cast<size_t>(grid_->cell_count()), 0.0);
	std::vector<double> column_theta_before(static_cast<size_t>(grid_->cell_count()), 0.0);
	std::vector<std::vector<double>> column_tracer_before(
		original.tracer_mass_kg_m2.size(),
		std::vector<double>(static_cast<size_t>(grid_->cell_count()), 0.0));

	for (int c = 0; c < grid_->cell_count(); ++c) {
		std::array<double, LEVELS> source_mass{};
		std::array<double, LEVELS> source_theta_mass{};
		std::array<double, LEVELS> target_mass{};
		double column_mass = 0.0;
		double column_theta = 0.0;
		for (int k = 0; k < LEVELS; ++k) {
			const int i = scalar_index(k, c);
			source_mass[static_cast<size_t>(k)] = original.layer_mass_kg_m2[static_cast<size_t>(i)];
			source_theta_mass[static_cast<size_t>(k)] = original.theta_mass_kg_k_m2[static_cast<size_t>(i)];
			column_mass += source_mass[static_cast<size_t>(k)];
			column_theta += source_theta_mass[static_cast<size_t>(k)];
			for (size_t tracer = 0; tracer < original.tracer_mass_kg_m2.size(); ++tracer) {
				column_tracer_before[tracer][static_cast<size_t>(c)]
					+= original.tracer_mass_kg_m2[tracer][static_cast<size_t>(i)];
			}
		}
		column_mass_before[static_cast<size_t>(c)] = column_mass;
		column_theta_before[static_cast<size_t>(c)] = column_theta;

		double assigned = 0.0;
		for (int k = 0; k < LEVELS - 1; ++k) {
			target_mass[static_cast<size_t>(k)]
				= column_mass * reference_mass_fraction_[static_cast<size_t>(k)];
			assigned += target_mass[static_cast<size_t>(k)];
		}
		target_mass[LEVELS - 1] = column_mass - assigned;
		const auto target_theta_mass = remap_extensive_by_mass_overlap(
			source_mass, source_theta_mass, target_mass);

		for (int k = 0; k < LEVELS; ++k) {
			const int i = scalar_index(k, c);
			state.layer_mass_kg_m2[static_cast<size_t>(i)] = target_mass[static_cast<size_t>(k)];
			state.theta_mass_kg_k_m2[static_cast<size_t>(i)] = target_theta_mass[static_cast<size_t>(k)];
		}

		for (size_t tracer = 0; tracer < original.tracer_mass_kg_m2.size(); ++tracer) {
			std::array<double, LEVELS> source_tracer_mass{};
			for (int k = 0; k < LEVELS; ++k) {
				source_tracer_mass[static_cast<size_t>(k)] = original.tracer_mass_kg_m2[tracer][
					static_cast<size_t>(scalar_index(k, c))];
			}
			const auto target_tracer_mass = remap_extensive_by_mass_overlap(
				source_mass, source_tracer_mass, target_mass);
			for (int k = 0; k < LEVELS; ++k) {
				state.tracer_mass_kg_m2[tracer][static_cast<size_t>(scalar_index(k, c))]
					= target_tracer_mass[static_cast<size_t>(k)];
			}
		}
	}

	// Remap edge-mass-weighted horizontal momentum with the same source/target
	// vertical mass coordinate.
	for (int e = 0; e < grid_->edge_count(); ++e) {
		const auto &edge = grid_->edge(e);
		std::array<double, LEVELS> source_edge_mass{};
		std::array<double, LEVELS> source_momentum{};
		std::array<double, LEVELS> target_edge_mass{};
		long double momentum_before = 0.0L;
		for (int k = 0; k < LEVELS; ++k) {
			const double ma = original.layer_mass_kg_m2[
				static_cast<size_t>(scalar_index(k, edge.cell_a))];
			const double mb = original.layer_mass_kg_m2[
				static_cast<size_t>(scalar_index(k, edge.cell_b))];
			const double mt_a = state.layer_mass_kg_m2[
				static_cast<size_t>(scalar_index(k, edge.cell_a))];
			const double mt_b = state.layer_mass_kg_m2[
				static_cast<size_t>(scalar_index(k, edge.cell_b))];
			source_edge_mass[static_cast<size_t>(k)] = 0.5 * (ma + mb);
			target_edge_mass[static_cast<size_t>(k)] = 0.5 * (mt_a + mt_b);
			const double momentum = source_edge_mass[static_cast<size_t>(k)]
				* original.edge_normal_mps[static_cast<size_t>(edge_index(k, e))];
			source_momentum[static_cast<size_t>(k)] = momentum;
			momentum_before += momentum;
		}
		const auto target_momentum = remap_extensive_by_mass_overlap(
			source_edge_mass, source_momentum, target_edge_mass);
		long double momentum_after = 0.0L;
		for (int k = 0; k < LEVELS; ++k) {
			if (!(target_edge_mass[static_cast<size_t>(k)] > 0.0)) {
				state = original;
				throw std::runtime_error("Dry coordinate remap produced non-positive edge mass");
			}
			state.edge_normal_mps[static_cast<size_t>(edge_index(k, e))]
				= target_momentum[static_cast<size_t>(k)] / target_edge_mass[static_cast<size_t>(k)];
			momentum_after += target_momentum[static_cast<size_t>(k)];
		}
		d.max_edge_momentum_error = std::max(d.max_edge_momentum_error,
			relative_error(static_cast<double>(momentum_after), static_cast<double>(momentum_before)));
	}

	if (!finite_positive(state)) {
		state = original;
		throw std::runtime_error("Dry coordinate remap produced invalid state");
	}

	const double global_mass_after = horizontal_.total_dry_mass_kg(state);
	const double global_theta_after = horizontal_.total_theta_mass_kg_k(state);
	d.relative_dry_mass_error = relative_error(global_mass_after, global_mass_before);
	d.relative_theta_mass_error = relative_error(global_theta_after, global_theta_before);
	for (size_t tracer = 0; tracer < global_tracer_before.size(); ++tracer) {
		d.max_relative_tracer_mass_error = std::max(d.max_relative_tracer_mass_error,
			relative_error(horizontal_.total_tracer_mass_kg(state, static_cast<int>(tracer)),
				global_tracer_before[tracer]));
	}
	d.min_layer_mass_kg_m2 = std::numeric_limits<double>::infinity();
	d.min_potential_temperature_k = std::numeric_limits<double>::infinity();

	for (int c = 0; c < grid_->cell_count(); ++c) {
		double column_mass_after = 0.0;
		double column_theta_after = 0.0;
		std::vector<double> column_tracer_after(state.tracer_mass_kg_m2.size(), 0.0);
		for (int k = 0; k < LEVELS; ++k) {
			const int i = scalar_index(k, c);
			const double mass = state.layer_mass_kg_m2[static_cast<size_t>(i)];
			const double theta_mass = state.theta_mass_kg_k_m2[static_cast<size_t>(i)];
			column_mass_after += mass;
			column_theta_after += theta_mass;
			for (size_t tracer = 0; tracer < state.tracer_mass_kg_m2.size(); ++tracer) {
				column_tracer_after[tracer] += state.tracer_mass_kg_m2[tracer][static_cast<size_t>(i)];
			}
			d.min_layer_mass_kg_m2 = std::min(d.min_layer_mass_kg_m2, mass);
			d.min_potential_temperature_k = std::min(
				d.min_potential_temperature_k, theta_mass / mass);
		}
		d.max_column_mass_error = std::max(d.max_column_mass_error,
			relative_error(column_mass_after, column_mass_before[static_cast<size_t>(c)]));
		d.max_column_theta_mass_error = std::max(d.max_column_theta_mass_error,
			relative_error(column_theta_after, column_theta_before[static_cast<size_t>(c)]));
		for (size_t tracer = 0; tracer < state.tracer_mass_kg_m2.size(); ++tracer) {
			d.max_column_tracer_mass_error = std::max(d.max_column_tracer_mass_error,
				relative_error(column_tracer_after[tracer],
					column_tracer_before[tracer][static_cast<size_t>(c)]));
		}
		for (int k = 0; k < LEVELS; ++k) {
			const double fraction = state.layer_mass_kg_m2[
				static_cast<size_t>(scalar_index(k, c))] / column_mass_after;
			d.max_mass_fraction_error = std::max(d.max_mass_fraction_error,
				std::abs(fraction - reference_mass_fraction_[static_cast<size_t>(k)]));
		}
	}

	if (d.relative_dry_mass_error > REMAP_REJECT_TOL
			|| d.relative_theta_mass_error > REMAP_REJECT_TOL
			|| d.max_relative_tracer_mass_error > REMAP_REJECT_TOL
			|| d.max_column_mass_error > REMAP_REJECT_TOL
			|| d.max_column_theta_mass_error > REMAP_REJECT_TOL
			|| d.max_column_tracer_mass_error > REMAP_REJECT_TOL
			|| d.max_edge_momentum_error > REMAP_REJECT_TOL) {
		state = original;
		throw std::runtime_error("Dry coordinate remap failed conservation gate");
	}
	return d;
}

} // namespace asterra::weather

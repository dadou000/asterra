#include "voronoi_shallow_water.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace asterra::weather {

namespace {

Vec3d tangent_basis_1(const Vec3d &radial) {
	const Vec3d reference = std::abs(radial.y) < 0.9
		? Vec3d{0.0, 1.0, 0.0} : Vec3d{1.0, 0.0, 0.0};
	return normalized(cross(reference, radial));
}

Vec3d edge_left_tangent(const GeodesicVoronoiGrid::EdgeGeometry &edge) {
	return normalized(cross(edge.midpoint, edge.normal_a_to_b));
}

} // namespace

VoronoiShallowWater::VoronoiShallowWater(const GeodesicVoronoiGrid &grid,
		double gravity_mps2, double rotation_rate_rad_s, Vec3d rotation_axis)
	: grid_(&grid), gravity_mps2_(gravity_mps2),
	  rotation_rate_rad_s_(rotation_rate_rad_s), rotation_axis_(normalized(rotation_axis)) {
	if (!(gravity_mps2_ > 0.0) || !std::isfinite(gravity_mps2_)) {
		throw std::invalid_argument("VoronoiShallowWater gravity must be finite and positive");
	}
	if (!std::isfinite(rotation_rate_rad_s_)) {
		throw std::invalid_argument("VoronoiShallowWater rotation rate must be finite");
	}
	if (grid.cell_count() <= 0 || grid.edge_count() <= 0 || grid.vertex_count() <= 0) {
		throw std::invalid_argument("VoronoiShallowWater requires a built geodesic grid");
	}
	build_reconstruction();
}

VoronoiShallowWater::State VoronoiShallowWater::make_uniform_state(double depth_m) const {
	if (!(depth_m > 0.0) || !std::isfinite(depth_m)) {
		throw std::invalid_argument("Voronoi shallow-water depth must be finite and positive");
	}
	State state;
	state.depth_m.assign(static_cast<size_t>(grid_->cell_count()), depth_m);
	state.edge_normal_mps.assign(static_cast<size_t>(grid_->edge_count()), 0.0);
	return state;
}

void VoronoiShallowWater::validate_shape(const State &state) const {
	if (state.depth_m.size() != static_cast<size_t>(grid_->cell_count())) {
		throw std::invalid_argument("Voronoi shallow-water depth array has wrong size");
	}
	if (state.edge_normal_mps.size() != static_cast<size_t>(grid_->edge_count())) {
		throw std::invalid_argument("Voronoi shallow-water velocity array has wrong size");
	}
}

bool VoronoiShallowWater::validate_finite_positive(const State &state) const {
	for (double h : state.depth_m) {
		if (!(h > 0.0) || !std::isfinite(h)) return false;
	}
	for (double u : state.edge_normal_mps) {
		if (!std::isfinite(u)) return false;
	}
	return true;
}

void VoronoiShallowWater::build_reconstruction() {
	reconstruction_.resize(static_cast<size_t>(grid_->cell_count()));
	for (int c = 0; c < grid_->cell_count(); ++c) {
		const auto &cell = grid_->cell(c);
		const Vec3d e1 = tangent_basis_1(cell.center);
		const Vec3d e2 = normalized(cross(cell.center, e1));
		double a00 = 0.0;
		double a01 = 0.0;
		double a11 = 0.0;
		std::vector<Vec3d> outward(cell.edges.size());
		std::vector<double> sign(cell.edges.size(), 0.0);
		for (size_t k = 0; k < cell.edges.size(); ++k) {
			const auto &edge = grid_->edge(cell.edges[k]);
			const int other = edge.cell_a == c ? edge.cell_b : edge.cell_a;
			sign[k] = edge.cell_a == c ? 1.0 : -1.0;
			outward[k] = normalized(project_tangent(grid_->cell(other).center, cell.center));
			const double nx = dot(outward[k], e1);
			const double ny = dot(outward[k], e2);
			a00 += nx * nx;
			a01 += nx * ny;
			a11 += ny * ny;
		}
		const double determinant = a00 * a11 - a01 * a01;
		if (!(std::abs(determinant) > 1.0e-12) || !std::isfinite(determinant)) {
			throw std::runtime_error("Voronoi edge reconstruction matrix is singular");
		}
		CellReconstruction &rec = reconstruction_[static_cast<size_t>(c)];
		rec.coefficient.resize(cell.edges.size());
		for (size_t k = 0; k < cell.edges.size(); ++k) {
			const double nx = dot(outward[k], e1);
			const double ny = dot(outward[k], e2);
			const double cx = sign[k] * (a11 * nx - a01 * ny) / determinant;
			const double cy = sign[k] * (a00 * ny - a01 * nx) / determinant;
			rec.coefficient[k] = e1 * cx + e2 * cy;
		}
	}
}

Vec3d VoronoiShallowWater::reconstruct_cell_vector_from_edges(int cell_id,
		const std::vector<double> &edge_scalar) const {
	const auto &cell = grid_->cell(cell_id);
	const auto &rec = reconstruction_[static_cast<size_t>(cell_id)];
	Vec3d vector{};
	for (size_t k = 0; k < cell.edges.size(); ++k) {
		vector += rec.coefficient[k] * edge_scalar[static_cast<size_t>(cell.edges[k])];
	}
	return project_tangent(vector, cell.center);
}

std::vector<Vec3d> VoronoiShallowWater::reconstruct_cell_velocity(const State &state) const {
	validate_shape(state);
	std::vector<Vec3d> out(static_cast<size_t>(grid_->cell_count()));
	for (int c = 0; c < grid_->cell_count(); ++c) {
		out[static_cast<size_t>(c)] = reconstruct_cell_vector_from_edges(c, state.edge_normal_mps);
	}
	return out;
}

std::vector<double> VoronoiShallowWater::reconstruct_vertex_relative_vorticity(const State &state) const {
	validate_shape(state);
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
			else throw std::runtime_error("Dual vertex is not an endpoint of its incident edge");
			circulation += static_cast<long double>(sign)
				* static_cast<long double>(edge.center_distance_m)
				* static_cast<long double>(state.edge_normal_mps[static_cast<size_t>(e)]);
		}
		zeta[static_cast<size_t>(v)] = static_cast<double>(
			circulation / static_cast<long double>(vertex.dual_area_m2));
	}
	return zeta;
}

std::vector<double> VoronoiShallowWater::reconstruct_vertex_potential_vorticity(const State &state) const {
	const std::vector<double> zeta = reconstruct_vertex_relative_vorticity(state);
	std::vector<double> q(static_cast<size_t>(grid_->vertex_count()), 0.0);
	for (int v = 0; v < grid_->vertex_count(); ++v) {
		const auto &vertex = grid_->vertex(v);
		long double weighted_h = 0.0L;
		for (int k = 0; k < 3; ++k) {
			weighted_h += static_cast<long double>(vertex.kite_area_m2[k])
				* static_cast<long double>(state.depth_m[static_cast<size_t>(vertex.cells[k])]);
		}
		const double h_vertex = static_cast<double>(
			weighted_h / static_cast<long double>(vertex.dual_area_m2));
		if (!(h_vertex > 0.0) || !std::isfinite(h_vertex)) {
			throw std::runtime_error("Invalid kite-weighted dual-vertex layer thickness");
		}
		const double f = 2.0 * rotation_rate_rad_s_ * dot(rotation_axis_, vertex.center);
		q[static_cast<size_t>(v)] = (zeta[static_cast<size_t>(v)] + f) / h_vertex;
	}
	return q;
}

std::vector<double> VoronoiShallowWater::cell_kinetic_energy(const State &state) const {
	std::vector<double> kinetic(static_cast<size_t>(grid_->cell_count()), 0.0);
	for (int c = 0; c < grid_->cell_count(); ++c) {
		const auto &cell = grid_->cell(c);
		long double sum = 0.0L;
		for (int e : cell.edges) {
			const auto &edge = grid_->edge(e);
			const double u = state.edge_normal_mps[static_cast<size_t>(e)];
			sum += static_cast<long double>(edge.edge_area_m2)
				* static_cast<long double>(u * u);
		}
		kinetic[static_cast<size_t>(c)] = static_cast<double>(
			sum / (4.0L * static_cast<long double>(cell.area_m2)));
	}
	return kinetic;
}

double VoronoiShallowWater::total_volume_m3(const State &state) const {
	validate_shape(state);
	long double total = 0.0L;
	for (int c = 0; c < grid_->cell_count(); ++c) {
		total += static_cast<long double>(grid_->cell(c).area_m2)
			* static_cast<long double>(state.depth_m[static_cast<size_t>(c)]);
	}
	return static_cast<double>(total);
}

double VoronoiShallowWater::total_energy(const State &state) const {
	validate_shape(state);
	long double potential = 0.0L;
	for (int c = 0; c < grid_->cell_count(); ++c) {
		const long double h = state.depth_m[static_cast<size_t>(c)];
		potential += 0.5L * static_cast<long double>(gravity_mps2_)
			* static_cast<long double>(grid_->cell(c).area_m2) * h * h;
	}
	long double kinetic = 0.0L;
	for (int e = 0; e < grid_->edge_count(); ++e) {
		const auto &edge = grid_->edge(e);
		const double h_edge = 0.5 * (state.depth_m[static_cast<size_t>(edge.cell_a)]
			+ state.depth_m[static_cast<size_t>(edge.cell_b)]);
		const double u = state.edge_normal_mps[static_cast<size_t>(e)];
		kinetic += 0.5L * static_cast<long double>(edge.edge_area_m2)
			* static_cast<long double>(h_edge) * static_cast<long double>(u * u);
	}
	return static_cast<double>(potential + kinetic);
}

double VoronoiShallowWater::max_courant(const State &state, double dt_s) const {
	validate_shape(state);
	if (!(dt_s >= 0.0) || !std::isfinite(dt_s)) {
		throw std::invalid_argument("Voronoi CFL timestep must be finite and non-negative");
	}
	double max_rate = 0.0;
	for (int e = 0; e < grid_->edge_count(); ++e) {
		const auto &edge = grid_->edge(e);
		const double h = std::max(state.depth_m[static_cast<size_t>(edge.cell_a)],
			state.depth_m[static_cast<size_t>(edge.cell_b)]);
		const double speed = std::abs(state.edge_normal_mps[static_cast<size_t>(e)])
			+ std::sqrt(gravity_mps2_ * h);
		max_rate = std::max(max_rate, speed / edge.center_distance_m);
	}
	const double wave_cfl = dt_s * max_rate;
	const double inertial_cfl = dt_s * 2.0 * std::abs(rotation_rate_rad_s_);
	return std::max(wave_cfl, inertial_cfl);
}

double VoronoiShallowWater::stable_dt(const State &state, double target_cfl,
		double maximum_dt_s) const {
	if (!(target_cfl > 0.0) || target_cfl > 0.8 || !std::isfinite(target_cfl)) {
		throw std::invalid_argument("Voronoi target CFL must be in (0, 0.8]");
	}
	if (!(maximum_dt_s > 0.0) || !std::isfinite(maximum_dt_s)) {
		throw std::invalid_argument("Voronoi maximum timestep must be finite and positive");
	}
	const double unit = max_courant(state, 1.0);
	if (!(unit > 0.0) || !std::isfinite(unit)) {
		throw std::runtime_error("Voronoi state has invalid characteristic speed");
	}
	return std::min(maximum_dt_s, target_cfl / unit);
}

VoronoiShallowWater::Tendencies VoronoiShallowWater::compute_tendencies(const State &state) const {
	validate_shape(state);
	if (!validate_finite_positive(state)) {
		throw std::runtime_error("Voronoi tendency received invalid state");
	}
	Tendencies t;
	t.depth_dt.assign(static_cast<size_t>(grid_->cell_count()), 0.0);
	t.edge_velocity_dt.assign(static_cast<size_t>(grid_->edge_count()), 0.0);
	t.mass_flux.resize(static_cast<size_t>(grid_->edge_count()));
	t.bernoulli.resize(static_cast<size_t>(grid_->cell_count()));
	t.q_edge.resize(static_cast<size_t>(grid_->edge_count()));

	for (int e = 0; e < grid_->edge_count(); ++e) {
		const auto &edge = grid_->edge(e);
		const double h_edge = 0.5 * (state.depth_m[static_cast<size_t>(edge.cell_a)]
			+ state.depth_m[static_cast<size_t>(edge.cell_b)]);
		t.mass_flux[static_cast<size_t>(e)] = h_edge * state.edge_normal_mps[static_cast<size_t>(e)];
		const double volume_rate = edge.edge_length_m * t.mass_flux[static_cast<size_t>(e)];
		t.depth_dt[static_cast<size_t>(edge.cell_a)] -= volume_rate / grid_->cell(edge.cell_a).area_m2;
		t.depth_dt[static_cast<size_t>(edge.cell_b)] += volume_rate / grid_->cell(edge.cell_b).area_m2;
	}

	const std::vector<double> kinetic = cell_kinetic_energy(state);
	for (int c = 0; c < grid_->cell_count(); ++c) {
		t.bernoulli[static_cast<size_t>(c)] = gravity_mps2_ * state.depth_m[static_cast<size_t>(c)]
			+ kinetic[static_cast<size_t>(c)];
	}

	const std::vector<double> q_vertex = reconstruct_vertex_potential_vorticity(state);
	for (int e = 0; e < grid_->edge_count(); ++e) {
		const auto &edge = grid_->edge(e);
		t.q_edge[static_cast<size_t>(e)] = 0.5 * (
			q_vertex[static_cast<size_t>(edge.vertex_a)]
			+ q_vertex[static_cast<size_t>(edge.vertex_b)]);
	}

	// Bernoulli gradient is exact on the orthogonal primal/dual pair. The direct
	// cell jump makes an alternating pressure/thickness field visible on every
	// edge, so there is no A-grid checkerboard null mode.
	for (int e = 0; e < grid_->edge_count(); ++e) {
		const auto &edge = grid_->edge(e);
		t.edge_velocity_dt[static_cast<size_t>(e)] = -(
			t.bernoulli[static_cast<size_t>(edge.cell_b)]
			- t.bernoulli[static_cast<size_t>(edge.cell_a)]) / edge.center_distance_m;
	}

	// TRiSK PV flux, matching MPAS sw_compute_tend exactly: the MPAS
	// weightsOnEdge orientation makes the PV-flux contribution positive in the
	// normal momentum tendency. Symmetric pairwise PV averaging preserves the
	// metric-skew work cancellation of the Thuburn/Ringler edge operator.
	for (int e = 0; e < grid_->edge_count(); ++e) {
		const auto &target = grid_->edge(e);
		if (target.reconstruction_edges.size() != target.reconstruction_weights.size()) {
			throw std::runtime_error("TRiSK edgesOnEdge/weightsOnEdge size mismatch");
		}
		double q_flux_perp = 0.0;
		for (size_t k = 0; k < target.reconstruction_edges.size(); ++k) {
			const int source = target.reconstruction_edges[k];
			const double q_pair = 0.5 * (t.q_edge[static_cast<size_t>(e)]
				+ t.q_edge[static_cast<size_t>(source)]);
			q_flux_perp += target.reconstruction_weights[k]
				* q_pair * t.mass_flux[static_cast<size_t>(source)];
		}
		t.edge_velocity_dt[static_cast<size_t>(e)] += q_flux_perp;
	}

	return t;
}

double VoronoiShallowWater::instantaneous_energy_tendency(const State &state) const {
	const Tendencies t = compute_tendencies(state);
	long double tendency = 0.0L;
	for (int c = 0; c < grid_->cell_count(); ++c) {
		tendency += static_cast<long double>(grid_->cell(c).area_m2)
			* static_cast<long double>(t.bernoulli[static_cast<size_t>(c)])
			* static_cast<long double>(t.depth_dt[static_cast<size_t>(c)]);
	}
	for (int e = 0; e < grid_->edge_count(); ++e) {
		tendency += static_cast<long double>(grid_->edge(e).edge_area_m2)
			* static_cast<long double>(t.mass_flux[static_cast<size_t>(e)])
			* static_cast<long double>(t.edge_velocity_dt[static_cast<size_t>(e)]);
	}
	return static_cast<double>(tendency);
}

bool VoronoiShallowWater::euler_stage(const State &input, State &output, double dt_s) const {
	try {
		const Tendencies t = compute_tendencies(input);
		output.depth_m.resize(input.depth_m.size());
		output.edge_normal_mps.resize(input.edge_normal_mps.size());
		for (size_t c = 0; c < input.depth_m.size(); ++c) {
			output.depth_m[c] = input.depth_m[c] + dt_s * t.depth_dt[c];
			if (!(output.depth_m[c] > 0.0) || !std::isfinite(output.depth_m[c])) return false;
		}
		for (size_t e = 0; e < input.edge_normal_mps.size(); ++e) {
			output.edge_normal_mps[e] = input.edge_normal_mps[e] + dt_s * t.edge_velocity_dt[e];
			if (!std::isfinite(output.edge_normal_mps[e])) return false;
		}
		return true;
	} catch (const std::exception &) {
		return false;
	}
}

bool VoronoiShallowWater::ssprk3_attempt(const State &initial, State &candidate, double dt_s) const {
	State s1, euler2, s2, euler3;
	if (!euler_stage(initial, s1, dt_s)) return false;
	if (!euler_stage(s1, euler2, dt_s)) return false;
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
	if (!euler_stage(s2, euler3, dt_s)) return false;
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
}

VoronoiShallowWater::StepDiagnostics VoronoiShallowWater::step(
		State &state, double requested_dt_s, double target_cfl, int max_retries) const {
	validate_shape(state);
	if (!(requested_dt_s > 0.0) || !std::isfinite(requested_dt_s)) {
		throw std::invalid_argument("Voronoi requested timestep must be finite and positive");
	}
	if (!(target_cfl > 0.0) || target_cfl > 0.8 || !std::isfinite(target_cfl)) {
		throw std::invalid_argument("Voronoi target CFL must be in (0,0.8]");
	}
	if (max_retries < 0) throw std::invalid_argument("Voronoi max_retries must be non-negative");
	if (!validate_finite_positive(state)) throw std::runtime_error("Voronoi step received invalid state");

	StepDiagnostics diag;
	diag.requested_dt_s = requested_dt_s;
	diag.mass_before_m3 = total_volume_m3(state);
	diag.energy_before_j_per_density = total_energy(state);
	const State initial = state;
	const double first_dt = stable_dt(initial, target_cfl, requested_dt_s);

	bool exact_rest = !initial.depth_m.empty();
	const double h0 = initial.depth_m.empty() ? 0.0 : initial.depth_m.front();
	for (double h : initial.depth_m) if (h != h0) { exact_rest = false; break; }
	if (exact_rest) for (double u : initial.edge_normal_mps) if (u != 0.0) { exact_rest = false; break; }
	if (exact_rest) {
		diag.accepted_dt_s = first_dt;
		diag.max_courant = max_courant(initial, first_dt);
		diag.mass_after_m3 = diag.mass_before_m3;
		diag.energy_after_j_per_density = diag.energy_before_j_per_density;
		diag.min_depth_m = h0;
		diag.max_depth_m = h0;
		return diag;
	}

	double trial_dt = first_dt;
	State candidate;
	bool accepted = false;
	for (int attempt = 0; attempt <= max_retries; ++attempt) {
		if (ssprk3_attempt(initial, candidate, trial_dt)) {
			const double candidate_cfl = max_courant(candidate, trial_dt);
			const double candidate_energy = total_energy(candidate);
			const double energy_change = std::abs(candidate_energy - diag.energy_before_j_per_density)
				/ std::max(std::abs(diag.energy_before_j_per_density), 1.0);
			// The spatial operator is energy compatible; this guard therefore catches
			// temporal under-resolution rather than masking spatial instability.
			const double temporal_energy_guard = std::max(2.0e-8, 0.02 * target_cfl * target_cfl * target_cfl * target_cfl);
			if (std::isfinite(candidate_cfl) && candidate_cfl <= target_cfl * 1.05
					&& std::isfinite(candidate_energy) && energy_change <= temporal_energy_guard) {
				accepted = true;
				break;
			}
		}
		++diag.rejected_steps;
		trial_dt *= 0.5;
		if (!(trial_dt > 1.0e-7)) break;
	}
	if (!accepted) {
		state = initial;
		throw std::runtime_error("Voronoi shallow-water timestep failed invariants after rollback retries");
	}

	state = std::move(candidate);
	diag.accepted_dt_s = trial_dt;
	diag.max_courant = max_courant(state, trial_dt);
	diag.mass_after_m3 = total_volume_m3(state);
	diag.relative_mass_error = std::abs(diag.mass_after_m3 - diag.mass_before_m3)
		/ std::max(std::abs(diag.mass_before_m3), 1.0);
	diag.energy_after_j_per_density = total_energy(state);
	diag.relative_energy_change = std::abs(diag.energy_after_j_per_density - diag.energy_before_j_per_density)
		/ std::max(std::abs(diag.energy_before_j_per_density), 1.0);
	diag.min_depth_m = std::numeric_limits<double>::infinity();
	diag.max_depth_m = -std::numeric_limits<double>::infinity();
	diag.max_speed_mps = 0.0;
	for (double h : state.depth_m) {
		diag.min_depth_m = std::min(diag.min_depth_m, h);
		diag.max_depth_m = std::max(diag.max_depth_m, h);
	}
	for (double u : state.edge_normal_mps) diag.max_speed_mps = std::max(diag.max_speed_mps, std::abs(u));
	return diag;
}

} // namespace asterra::weather

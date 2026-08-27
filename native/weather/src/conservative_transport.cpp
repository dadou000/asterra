#include "conservative_transport.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace asterra::weather {

ConservativeTransport2D::ConservativeTransport2D(const CubedSphereGrid &grid) : grid_(&grid) {
	if (grid.cell_count() <= 0) {
		throw std::invalid_argument("ConservativeTransport2D requires a built cubed-sphere grid");
	}

	edges_.reserve(static_cast<size_t>(grid.cell_count()) * 2u);
	for (int cell_id = 0; cell_id < grid.cell_count(); ++cell_id) {
		const auto &cell = grid.cell(cell_id);
		for (int edge = 0; edge < CubedSphereGrid::EDGE_COUNT; ++edge) {
			const auto &n = cell.neighbour[edge];
			if (cell_id >= n.cell) continue;
			const auto &other = grid.cell(n.cell);

			SharedEdge shared;
			shared.cell_a = cell_id;
			shared.edge_a = edge;
			shared.cell_b = n.cell;
			shared.edge_b = n.edge;
			shared.length_m = 0.5 * (cell.edge_length_m[edge] + other.edge_length_m[n.edge]);
			shared.midpoint = normalized(cell.edge_midpoint[edge] + other.edge_midpoint[n.edge]);
			Vec3d normal = normalized(cell.outward_normal[edge] - other.outward_normal[n.edge]);
			if (dot(normal, cell.outward_normal[edge]) < 0.0) normal = normal * -1.0;
			shared.normal_a_to_b = normal;
			edges_.push_back(shared);
		}
	}

	const size_t expected = static_cast<size_t>(grid.cell_count()) * 2u;
	if (edges_.size() != expected) {
		throw std::runtime_error("Cubed-sphere shared-edge count is not 2*cells");
	}
}

std::vector<double> ConservativeTransport2D::sample_edge_normal_velocity(
		const VelocityFunction &velocity) const {
	if (!velocity) throw std::invalid_argument("Velocity function is empty");
	std::vector<double> out(edges_.size(), 0.0);
	for (size_t e = 0; e < edges_.size(); ++e) {
		const Vec3d v = velocity(edges_[e].midpoint);
		if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) {
			throw std::runtime_error("Velocity function returned a non-finite vector");
		}
		out[e] = dot(v, edges_[e].normal_a_to_b);
	}
	return out;
}

double ConservativeTransport2D::max_courant(
		const std::vector<double> &edge_normal_velocity_mps, double dt_s) const {
	if (edge_normal_velocity_mps.size() != edges_.size()) {
		throw std::invalid_argument("Edge velocity array has the wrong size");
	}
	if (!(dt_s >= 0.0) || !std::isfinite(dt_s)) {
		throw std::invalid_argument("CFL timestep must be finite and non-negative");
	}

	std::vector<double> outgoing_area_rate(static_cast<size_t>(grid_->cell_count()), 0.0);
	for (size_t e = 0; e < edges_.size(); ++e) {
		const double un = edge_normal_velocity_mps[e];
		if (!std::isfinite(un)) throw std::runtime_error("Non-finite edge-normal velocity");
		const double swept_area_rate = un * edges_[e].length_m;
		if (swept_area_rate >= 0.0) outgoing_area_rate[edges_[e].cell_a] += swept_area_rate;
		else outgoing_area_rate[edges_[e].cell_b] += -swept_area_rate;
	}

	double max_cfl = 0.0;
	for (int c = 0; c < grid_->cell_count(); ++c) {
		max_cfl = std::max(max_cfl, dt_s * outgoing_area_rate[c] / grid_->cell(c).area_m2);
	}
	return max_cfl;
}

double ConservativeTransport2D::stable_dt(
		const std::vector<double> &edge_normal_velocity_mps,
		double target_cfl,
		double maximum_dt_s) const {
	if (!(target_cfl > 0.0) || !std::isfinite(target_cfl)) {
		throw std::invalid_argument("Target CFL must be finite and positive");
	}
	if (!(maximum_dt_s > 0.0)) {
		throw std::invalid_argument("Maximum timestep must be positive");
	}
	if (edge_normal_velocity_mps.size() != edges_.size()) {
		throw std::invalid_argument("Edge velocity array has the wrong size");
	}

	std::vector<double> outgoing_area_rate(static_cast<size_t>(grid_->cell_count()), 0.0);
	for (size_t e = 0; e < edges_.size(); ++e) {
		const double un = edge_normal_velocity_mps[e];
		if (!std::isfinite(un)) throw std::runtime_error("Non-finite edge-normal velocity");
		const double swept_area_rate = un * edges_[e].length_m;
		if (swept_area_rate >= 0.0) outgoing_area_rate[edges_[e].cell_a] += swept_area_rate;
		else outgoing_area_rate[edges_[e].cell_b] += -swept_area_rate;
	}

	double max_rate = 0.0;
	for (int c = 0; c < grid_->cell_count(); ++c) {
		max_rate = std::max(max_rate, outgoing_area_rate[c] / grid_->cell(c).area_m2);
	}
	if (!(max_rate > 0.0)) return maximum_dt_s;
	return std::min(target_cfl / max_rate, maximum_dt_s);
}

double ConservativeTransport2D::total_mass(const std::vector<double> &density) const {
	if (density.size() != static_cast<size_t>(grid_->cell_count())) {
		throw std::invalid_argument("Density array has the wrong size");
	}
	long double total = 0.0L;
	for (int c = 0; c < grid_->cell_count(); ++c) {
		if (!std::isfinite(density[c])) throw std::runtime_error("Density contains NaN/Inf");
		total += static_cast<long double>(density[c])
			* static_cast<long double>(grid_->cell(c).area_m2);
	}
	return static_cast<double>(total);
}

int ConservativeTransport2D::opposite_edge(int edge) {
	switch (edge) {
		case CubedSphereGrid::WEST: return CubedSphereGrid::EAST;
		case CubedSphereGrid::EAST: return CubedSphereGrid::WEST;
		case CubedSphereGrid::SOUTH: return CubedSphereGrid::NORTH;
		case CubedSphereGrid::NORTH: return CubedSphereGrid::SOUTH;
		default: throw std::out_of_range("Invalid cubed-sphere edge index");
	}
}

double ConservativeTransport2D::great_circle_distance_m(
		const Vec3d &a, const Vec3d &b, double radius_m) {
	const double angle = std::atan2(length(cross(a, b)), std::clamp(dot(a, b), -1.0, 1.0));
	return angle * radius_m;
}

double ConservativeTransport2D::minmod3(double a, double b, double c) {
	if (a > 0.0 && b > 0.0 && c > 0.0) return std::min(a, std::min(b, c));
	if (a < 0.0 && b < 0.0 && c < 0.0) return std::max(a, std::max(b, c));
	return 0.0;
}

double ConservativeTransport2D::reconstruct_outflow_density(
		const std::vector<double> &density,
		int donor_cell, int donor_edge, int receiver_cell,
		const Vec3d &edge_midpoint,
		Reconstruction reconstruction) const {
	const double q0 = density[donor_cell];
	const double q_front = density[receiver_cell];
	if (!std::isfinite(q0) || !std::isfinite(q_front) || q0 < 0.0 || q_front < 0.0) {
		throw std::runtime_error("Flux reconstruction received an invalid density");
	}
	if (reconstruction == Reconstruction::DONOR_CELL) return q0;

	const auto &donor = grid_->cell(donor_cell);
	const int behind_edge = opposite_edge(donor_edge);
	const int behind_cell = donor.neighbour[behind_edge].cell;
	const double q_back = density[behind_cell];
	if (!std::isfinite(q_back) || q_back < 0.0) {
		throw std::runtime_error("MUSCL reconstruction received an invalid upwind density");
	}

	const double radius = grid_->radius_m();
	const double d_back = std::max(great_circle_distance_m(
		grid_->cell(behind_cell).center, donor.center, radius), 1.0);
	const double d_front = std::max(great_circle_distance_m(
		donor.center, grid_->cell(receiver_cell).center, radius), 1.0);
	const double d_face = std::max(great_circle_distance_m(
		donor.center, edge_midpoint, radius), 0.0);

	const double grad_back = (q0 - q_back) / d_back;
	const double grad_front = (q_front - q0) / d_front;
	const double grad_center = (d_front * grad_back + d_back * grad_front)
		/ (d_back + d_front);
	const double limited_grad = minmod3(2.0 * grad_back, grad_center, 2.0 * grad_front);
	double face_value = q0 + limited_grad * d_face;

	const double local_min = std::min(q0, std::min(q_back, q_front));
	const double local_max = std::max(q0, std::max(q_back, q_front));
	face_value = std::clamp(face_value, local_min, local_max);
	return std::max(face_value, 0.0);
}

std::size_t ConservativeTransport2D::euler_stage(
		const std::vector<double> &input,
		std::vector<double> &output,
		const std::vector<double> &edge_normal_velocity_mps,
		double dt_s,
		Reconstruction reconstruction) const {
	const size_t cell_count = static_cast<size_t>(grid_->cell_count());
	if (input.size() != cell_count || edge_normal_velocity_mps.size() != edges_.size()) {
		throw std::invalid_argument("Euler transport stage array size mismatch");
	}
	if (!(dt_s > 0.0) || !std::isfinite(dt_s)) {
		throw std::invalid_argument("Euler transport timestep must be finite and positive");
	}

	std::vector<double> raw_mass_flux(edges_.size(), 0.0);
	std::vector<double> outgoing_mass_rate(cell_count, 0.0);

	for (size_t e = 0; e < edges_.size(); ++e) {
		const SharedEdge &edge = edges_[e];
		const double volume_flux = edge_normal_velocity_mps[e] * edge.length_m;
		if (volume_flux == 0.0) continue;

		const bool a_donor = volume_flux > 0.0;
		const int donor = a_donor ? edge.cell_a : edge.cell_b;
		const int receiver = a_donor ? edge.cell_b : edge.cell_a;
		const int donor_edge = a_donor ? edge.edge_a : edge.edge_b;
		const double face_density = reconstruct_outflow_density(
			input, donor, donor_edge, receiver, edge.midpoint, reconstruction);
		const double flux = volume_flux * face_density;
		raw_mass_flux[e] = flux;
		outgoing_mass_rate[donor] += std::abs(flux);
	}

	std::vector<double> donor_scale(cell_count, 1.0);
	std::size_t limiter_activations = 0;
	constexpr double ROUND_OFF_RESERVE = 64.0 * std::numeric_limits<double>::epsilon();
	for (int c = 0; c < grid_->cell_count(); ++c) {
		const double q = input[c];
		if (!std::isfinite(q) || q < 0.0) {
			throw std::runtime_error("Transport input must be finite and non-negative");
		}
		const double available_mass = q * grid_->cell(c).area_m2;
		const double requested_outflow = outgoing_mass_rate[c] * dt_s;
		const double safe_mass = available_mass * (1.0 - ROUND_OFF_RESERVE);
		if (requested_outflow > safe_mass && requested_outflow > 0.0) {
			donor_scale[c] = std::max(safe_mass, 0.0) / requested_outflow;
			++limiter_activations;
		}
	}

	std::vector<double> delta_mass(cell_count, 0.0);
	for (size_t e = 0; e < edges_.size(); ++e) {
		const SharedEdge &edge = edges_[e];
		const double raw = raw_mass_flux[e];
		if (raw == 0.0) continue;
		const int donor = raw > 0.0 ? edge.cell_a : edge.cell_b;
		const double transferred_mass = raw * donor_scale[donor] * dt_s;
		delta_mass[edge.cell_a] -= transferred_mass;
		delta_mass[edge.cell_b] += transferred_mass;
	}

	output.resize(cell_count);
	for (int c = 0; c < grid_->cell_count(); ++c) {
		const double area = grid_->cell(c).area_m2;
		double new_mass = input[c] * area + delta_mass[c];
		const double tolerance = 128.0 * std::numeric_limits<double>::epsilon()
			* std::max(input[c] * area, 1.0);
		if (new_mass < -tolerance || !std::isfinite(new_mass)) {
			throw std::runtime_error("Conservative transport produced invalid/negative mass");
		}
		if (new_mass < 0.0) new_mass = 0.0;
		output[c] = new_mass / area;
	}
	return limiter_activations;
}

ConservativeTransport2D::StepDiagnostics ConservativeTransport2D::step_ssprk3(
		std::vector<double> &density,
		const std::vector<double> &edge_normal_velocity_mps,
		double requested_dt_s,
		double target_cfl,
		Reconstruction reconstruction) const {
	if (density.size() != static_cast<size_t>(grid_->cell_count())) {
		throw std::invalid_argument("Density array has the wrong size");
	}
	if (!(requested_dt_s > 0.0) || !std::isfinite(requested_dt_s)) {
		throw std::invalid_argument("Requested timestep must be finite and positive");
	}

	StepDiagnostics diagnostics;
	diagnostics.requested_dt_s = requested_dt_s;
	diagnostics.mass_before = total_mass(density);
	const double cfl_dt = stable_dt(edge_normal_velocity_mps, target_cfl, requested_dt_s);
	diagnostics.used_dt_s = std::min(requested_dt_s, cfl_dt);
	diagnostics.max_courant = max_courant(edge_normal_velocity_mps, diagnostics.used_dt_s);

	if (diagnostics.max_courant == 0.0) {
		diagnostics.mass_after = diagnostics.mass_before;
		diagnostics.relative_mass_error = 0.0;
		diagnostics.min_density = std::numeric_limits<double>::infinity();
		diagnostics.max_density = -std::numeric_limits<double>::infinity();
		for (double q : density) {
			if (!std::isfinite(q) || q < 0.0) {
				throw std::runtime_error("Transport state is invalid even though flux is zero");
			}
			diagnostics.min_density = std::min(diagnostics.min_density, q);
			diagnostics.max_density = std::max(diagnostics.max_density, q);
		}
		return diagnostics;
	}

	const std::vector<double> u0 = density;
	std::vector<double> u1, euler2, u2, euler3;
	diagnostics.positivity_limiter_activations += euler_stage(
		u0, u1, edge_normal_velocity_mps, diagnostics.used_dt_s, reconstruction);
	diagnostics.positivity_limiter_activations += euler_stage(
		u1, euler2, edge_normal_velocity_mps, diagnostics.used_dt_s, reconstruction);

	u2.resize(u0.size());
	for (size_t i = 0; i < u0.size(); ++i) {
		u2[i] = 0.75 * u0[i] + 0.25 * euler2[i];
	}

	diagnostics.positivity_limiter_activations += euler_stage(
		u2, euler3, edge_normal_velocity_mps, diagnostics.used_dt_s, reconstruction);
	for (size_t i = 0; i < u0.size(); ++i) {
		density[i] = (1.0 / 3.0) * u0[i] + (2.0 / 3.0) * euler3[i];
	}

	diagnostics.min_density = std::numeric_limits<double>::infinity();
	diagnostics.max_density = -std::numeric_limits<double>::infinity();
	for (double q : density) {
		if (!std::isfinite(q) || q < 0.0) {
			throw std::runtime_error("SSPRK3 transport produced an invalid state");
		}
		diagnostics.min_density = std::min(diagnostics.min_density, q);
		diagnostics.max_density = std::max(diagnostics.max_density, q);
	}
	diagnostics.mass_after = total_mass(density);
	diagnostics.relative_mass_error = std::abs(diagnostics.mass_after - diagnostics.mass_before)
		/ std::max(std::abs(diagnostics.mass_before), 1.0);
	return diagnostics;
}

} // namespace asterra::weather

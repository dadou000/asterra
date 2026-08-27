#include "weather_core_native.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace godot {

using asterra::weather::GeodesicVoronoiGrid;
using asterra::weather::Vec3d;
using asterra::weather::VoronoiShallowWater;
using asterra::weather::cross;
using asterra::weather::dot;
using asterra::weather::normalized;

namespace {
constexpr double PI = 3.141592653589793238462643383279502884;

bool finite_positive(double x) {
	return std::isfinite(x) && x > 0.0;
}
} // namespace

void WeatherCoreNative::_bind_methods() {
	ClassDB::bind_method(D_METHOD("initialize", "frequency", "base_depth_m"),
		&WeatherCoreNative::initialize, DEFVAL(64), DEFVAL(5000.0));
	ClassDB::bind_method(D_METHOD("step", "requested_dt_s", "target_cfl"),
		&WeatherCoreNative::step, DEFVAL(0.30));
	ClassDB::bind_method(D_METHOD("reset_balanced_flow", "base_depth_m", "flow_rate_fraction"),
		&WeatherCoreNative::reset_balanced_flow, DEFVAL(5000.0), DEFVAL(1.0 / 12.0));
	ClassDB::bind_method(D_METHOD("reset_rest", "depth_m"),
		&WeatherCoreNative::reset_rest, DEFVAL(5000.0));
	ClassDB::bind_method(D_METHOD("add_height_perturbation", "center_direction", "amplitude_m", "angular_radius_rad"),
		&WeatherCoreNative::add_height_perturbation);
	ClassDB::bind_method(D_METHOD("set_rotation_axis", "axis"), &WeatherCoreNative::set_rotation_axis);
	ClassDB::bind_method(D_METHOD("get_rotation_axis"), &WeatherCoreNative::get_rotation_axis);
	ClassDB::bind_method(D_METHOD("set_rotation_rate", "rate_rad_s"), &WeatherCoreNative::set_rotation_rate);
	ClassDB::bind_method(D_METHOD("get_rotation_rate"), &WeatherCoreNative::get_rotation_rate);
	ClassDB::bind_method(D_METHOD("get_global_core_rgba"), &WeatherCoreNative::get_global_core_rgba);
	ClassDB::bind_method(D_METHOD("get_runtime_diagnostics"), &WeatherCoreNative::get_runtime_diagnostics);
	ClassDB::bind_method(D_METHOD("get_frequency"), &WeatherCoreNative::get_frequency);
	ClassDB::bind_method(D_METHOD("get_cell_count"), &WeatherCoreNative::get_cell_count);
	ClassDB::bind_method(D_METHOD("get_edge_count"), &WeatherCoreNative::get_edge_count);
	ClassDB::bind_method(D_METHOD("get_display_width"), &WeatherCoreNative::get_display_width);
	ClassDB::bind_method(D_METHOD("get_display_height"), &WeatherCoreNative::get_display_height);
	ClassDB::bind_method(D_METHOD("get_simulation_seconds"), &WeatherCoreNative::get_simulation_seconds);
}

Vec3d WeatherCoreNative::to_vec3d(const Vector3 &v) {
	return {static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z)};
}

Vector3 WeatherCoreNative::to_vector3(const Vec3d &v) {
	return Vector3(static_cast<real_t>(v.x), static_cast<real_t>(v.y), static_cast<real_t>(v.z));
}

void WeatherCoreNative::initialize(int p_frequency, double p_base_depth_m) {
	if (p_frequency < 1 || p_frequency > 256) {
		UtilityFunctions::push_error("WeatherCoreNative frequency must be in [1, 256]");
		return;
	}
	if (!finite_positive(p_base_depth_m)) {
		UtilityFunctions::push_error("WeatherCoreNative base depth must be finite and positive");
		return;
	}

	try {
		auto new_grid = std::make_unique<GeodesicVoronoiGrid>(p_frequency, PLANET_RADIUS_M);
		auto new_solver = std::make_unique<VoronoiShallowWater>(
			*new_grid, GRAVITY_MPS2, rotation_rate_rad_s_, rotation_axis_);
		auto new_state = new_solver->make_uniform_state(p_base_depth_m);

		grid_ = std::move(new_grid);
		solver_ = std::move(new_solver);
		state_ = std::move(new_state);
		frequency_ = p_frequency;
		simulation_seconds_ = 0.0;
		rejected_steps_total_ = 0;
		last_step_ = {};
		rebuild_display_lookup();
		reset_budget_baseline();
		last_step_.min_depth_m = p_base_depth_m;
		last_step_.max_depth_m = p_base_depth_m;
	} catch (const std::exception &e) {
		grid_.reset();
		solver_.reset();
		state_ = {};
		display_cell_lookup_.clear();
		frequency_ = 0;
		UtilityFunctions::push_error(String("WeatherCoreNative initialization failed: ") + e.what());
	}
}

double WeatherCoreNative::step(double requested_dt_s, double target_cfl) {
	if (!ready()) {
		UtilityFunctions::push_error("WeatherCoreNative.step called before initialize");
		return 0.0;
	}
	if (!finite_positive(requested_dt_s) || !finite_positive(target_cfl) || target_cfl > 0.45) {
		UtilityFunctions::push_error("WeatherCoreNative step requires dt > 0 and target_cfl in (0, 0.45]");
		return 0.0;
	}

	try {
		last_step_ = solver_->step(state_, requested_dt_s, target_cfl, 10);
		rejected_steps_total_ += last_step_.rejected_steps;
		if (last_step_.accepted_dt_s > 0.0) simulation_seconds_ += last_step_.accepted_dt_s;
		return last_step_.accepted_dt_s;
	} catch (const std::exception &e) {
		UtilityFunctions::push_error(String("WeatherCoreNative step failed: ") + e.what());
		return 0.0;
	}
}

void WeatherCoreNative::reset_rest(double depth_m) {
	if (!ready()) {
		UtilityFunctions::push_error("WeatherCoreNative.reset_rest called before initialize");
		return;
	}
	if (!finite_positive(depth_m)) {
		UtilityFunctions::push_error("WeatherCoreNative rest depth must be finite and positive");
		return;
	}
	state_ = solver_->make_uniform_state(depth_m);
	simulation_seconds_ = 0.0;
	rejected_steps_total_ = 0;
	last_step_ = {};
	last_step_.min_depth_m = depth_m;
	last_step_.max_depth_m = depth_m;
	reset_budget_baseline();
}

void WeatherCoreNative::reset_balanced_flow(double base_depth_m, double flow_rate_fraction) {
	if (!ready()) {
		UtilityFunctions::push_error("WeatherCoreNative.reset_balanced_flow called before initialize");
		return;
	}
	if (!finite_positive(base_depth_m) || !std::isfinite(flow_rate_fraction)) {
		UtilityFunctions::push_error("WeatherCoreNative balanced-flow parameters must be finite");
		return;
	}

	const double flow_rate = rotation_rate_rad_s_ * flow_rate_fraction;
	const double u0 = flow_rate * PLANET_RADIUS_M;
	const double amplitude = (rotation_rate_rad_s_ * flow_rate + 0.5 * flow_rate * flow_rate)
		* PLANET_RADIUS_M * PLANET_RADIUS_M / GRAVITY_MPS2;
	if (!(base_depth_m - std::abs(amplitude) > 0.0)) {
		UtilityFunctions::push_error("WeatherCoreNative balanced flow would create non-positive layer depth");
		return;
	}

	auto candidate = solver_->make_uniform_state(base_depth_m);
	for (int c = 0; c < grid_->cell_count(); ++c) {
		const double mu = dot(rotation_axis_, grid_->cell(c).center);
		candidate.depth_m[static_cast<size_t>(c)] = base_depth_m - amplitude * mu * mu;
	}
	for (int e = 0; e < grid_->edge_count(); ++e) {
		const auto &edge = grid_->edge(e);
		const Vec3d analytic = cross(rotation_axis_, edge.midpoint) * u0;
		candidate.edge_normal_mps[static_cast<size_t>(e)] = dot(analytic, edge.normal_a_to_b);
	}

	state_ = std::move(candidate);
	simulation_seconds_ = 0.0;
	rejected_steps_total_ = 0;
	last_step_ = {};
	last_step_.min_depth_m = *std::min_element(state_.depth_m.begin(), state_.depth_m.end());
	last_step_.max_depth_m = *std::max_element(state_.depth_m.begin(), state_.depth_m.end());
	for (double u : state_.edge_normal_mps) last_step_.max_speed_mps = std::max(last_step_.max_speed_mps, std::abs(u));
	reset_budget_baseline();
}

bool WeatherCoreNative::add_height_perturbation(const Vector3 &center_direction,
		double amplitude_m, double angular_radius_rad) {
	if (!ready()) {
		UtilityFunctions::push_error("WeatherCoreNative.add_height_perturbation called before initialize");
		return false;
	}
	if (!std::isfinite(amplitude_m) || !finite_positive(angular_radius_rad) || angular_radius_rad > PI) {
		UtilityFunctions::push_error("WeatherCoreNative perturbation parameters are invalid");
		return false;
	}
	const Vec3d raw_center = to_vec3d(center_direction);
	const double n2 = dot(raw_center, raw_center);
	if (!(n2 > 1.0e-24) || !std::isfinite(n2)) {
		UtilityFunctions::push_error("WeatherCoreNative perturbation center must be a finite non-zero direction");
		return false;
	}
	const Vec3d center = raw_center / std::sqrt(n2);

	std::vector<double> delta(static_cast<size_t>(grid_->cell_count()), 0.0);
	for (int c = 0; c < grid_->cell_count(); ++c) {
		const double cosine = std::clamp(dot(center, grid_->cell(c).center), -1.0, 1.0);
		const double angle = std::acos(cosine);
		if (angle >= angular_radius_rad) continue;
		const double bell = 0.5 * (1.0 + std::cos(PI * angle / angular_radius_rad));
		delta[static_cast<size_t>(c)] = amplitude_m * bell;
		const double candidate = state_.depth_m[static_cast<size_t>(c)] + delta[static_cast<size_t>(c)];
		if (!finite_positive(candidate)) {
			UtilityFunctions::push_error("WeatherCoreNative perturbation would create non-positive depth; state unchanged");
			return false;
		}
	}
	for (int c = 0; c < grid_->cell_count(); ++c) {
		state_.depth_m[static_cast<size_t>(c)] += delta[static_cast<size_t>(c)];
	}
	last_step_ = {};
	last_step_.min_depth_m = *std::min_element(state_.depth_m.begin(), state_.depth_m.end());
	last_step_.max_depth_m = *std::max_element(state_.depth_m.begin(), state_.depth_m.end());
	for (double u : state_.edge_normal_mps) last_step_.max_speed_mps = std::max(last_step_.max_speed_mps, std::abs(u));
	reset_budget_baseline();
	return true;
}

void WeatherCoreNative::set_rotation_axis(const Vector3 &axis) {
	const Vec3d raw = to_vec3d(axis);
	const double n2 = dot(raw, raw);
	if (!(n2 > 1.0e-24) || !std::isfinite(n2)) {
		UtilityFunctions::push_error("WeatherCoreNative rotation axis must be a finite non-zero vector");
		return;
	}
	rotation_axis_ = raw / std::sqrt(n2);
	if (grid_) {
		solver_ = std::make_unique<VoronoiShallowWater>(
			*grid_, GRAVITY_MPS2, rotation_rate_rad_s_, rotation_axis_);
		reset_budget_baseline();
	}
}

void WeatherCoreNative::set_rotation_rate(double rate_rad_s) {
	if (!std::isfinite(rate_rad_s)) {
		UtilityFunctions::push_error("WeatherCoreNative rotation rate must be finite");
		return;
	}
	rotation_rate_rad_s_ = rate_rad_s;
	if (grid_) {
		solver_ = std::make_unique<VoronoiShallowWater>(
			*grid_, GRAVITY_MPS2, rotation_rate_rad_s_, rotation_axis_);
		reset_budget_baseline();
	}
}

int WeatherCoreNative::nearest_cell_hill_climb(const Vec3d &direction, int seed) const {
	if (!grid_ || grid_->cell_count() == 0) return -1;
	int current = (seed >= 0 && seed < grid_->cell_count()) ? seed : 0;
	double best = dot(direction, grid_->cell(current).center);
	for (int iteration = 0; iteration < grid_->cell_count(); ++iteration) {
		int next = current;
		double next_best = best;
		for (int neighbour : grid_->cell(current).neighbours) {
			const double score = dot(direction, grid_->cell(neighbour).center);
			if (score > next_best + 1.0e-15) {
				next = neighbour;
				next_best = score;
			}
		}
		if (next == current) return current;
		current = next;
		best = next_best;
	}
	return current;
}

void WeatherCoreNative::rebuild_display_lookup() {
	display_cell_lookup_.assign(static_cast<size_t>(DISPLAY_W * DISPLAY_H), -1);
	if (!grid_ || grid_->cell_count() == 0) return;

	int previous_row_seed = 0;
	for (int y = 0; y < DISPLAY_H; ++y) {
		const double lat = 0.5 * PI - PI * (static_cast<double>(y) + 0.5) / DISPLAY_H;
		const double cos_lat = std::cos(lat);
		const double sin_lat = std::sin(lat);
		int current = previous_row_seed;
		int first_in_row = current;
		for (int x = 0; x < DISPLAY_W; ++x) {
			const double lon = -PI + 2.0 * PI * (static_cast<double>(x) + 0.5) / DISPLAY_W;
			const Vec3d direction{cos_lat * std::cos(lon), sin_lat, cos_lat * std::sin(lon)};
			current = nearest_cell_hill_climb(direction, current);
			display_cell_lookup_[static_cast<size_t>(x + y * DISPLAY_W)] = current;
			if (x == 0) first_in_row = current;
		}
		previous_row_seed = first_in_row;
	}
}

void WeatherCoreNative::reset_budget_baseline() {
	if (!ready()) {
		initial_mass_m3_ = 0.0;
		initial_energy_ = 0.0;
		return;
	}
	initial_mass_m3_ = solver_->total_volume_m3(state_);
	initial_energy_ = solver_->total_energy(state_);
}

PackedFloat32Array WeatherCoreNative::get_global_core_rgba() const {
	PackedFloat32Array out;
	if (!ready() || display_cell_lookup_.size() != static_cast<size_t>(DISPLAY_W * DISPLAY_H)) return out;

	const std::vector<Vec3d> velocity = solver_->reconstruct_cell_velocity(state_);
	const std::vector<double> vertex_vorticity = solver_->reconstruct_vertex_relative_vorticity(state_);
	std::vector<double> cell_vorticity(static_cast<size_t>(grid_->cell_count()), 0.0);
	for (int c = 0; c < grid_->cell_count(); ++c) {
		const auto &cell = grid_->cell(c);
		long double sum = 0.0L;
		for (int v : cell.vertices) sum += vertex_vorticity[static_cast<size_t>(v)];
		cell_vorticity[static_cast<size_t>(c)] = cell.vertices.empty()
			? 0.0 : static_cast<double>(sum / static_cast<long double>(cell.vertices.size()));
	}

	out.resize(DISPLAY_W * DISPLAY_H * 4);
	float *dst = out.ptrw();
	for (int p = 0; p < DISPLAY_W * DISPLAY_H; ++p) {
		const int c = display_cell_lookup_[static_cast<size_t>(p)];
		const Vec3d &u = velocity[static_cast<size_t>(c)];
		dst[4 * p + 0] = static_cast<float>(state_.depth_m[static_cast<size_t>(c)]);
		dst[4 * p + 1] = static_cast<float>(std::sqrt(dot(u, u)));
		dst[4 * p + 2] = static_cast<float>(cell_vorticity[static_cast<size_t>(c)]);
		dst[4 * p + 3] = 1.0f;
	}
	return out;
}

PackedFloat32Array WeatherCoreNative::get_runtime_diagnostics() const {
	PackedFloat32Array out;
	if (!ready()) return out;
	out.resize(12);
	float *dst = out.ptrw();
	const double mass = solver_->total_volume_m3(state_);
	const double energy = solver_->total_energy(state_);
	const double mass_drift = initial_mass_m3_ != 0.0 ? (mass - initial_mass_m3_) / initial_mass_m3_ : 0.0;
	const double energy_drift = initial_energy_ != 0.0 ? (energy - initial_energy_) / std::abs(initial_energy_) : 0.0;

	dst[0] = static_cast<float>(simulation_seconds_);
	dst[1] = static_cast<float>(last_step_.requested_dt_s);
	dst[2] = static_cast<float>(last_step_.accepted_dt_s);
	dst[3] = static_cast<float>(last_step_.max_courant);
	dst[4] = static_cast<float>(rejected_steps_total_);
	dst[5] = static_cast<float>(mass_drift);
	dst[6] = static_cast<float>(energy_drift);
	dst[7] = static_cast<float>(last_step_.min_depth_m);
	dst[8] = static_cast<float>(last_step_.max_depth_m);
	dst[9] = static_cast<float>(last_step_.max_speed_mps);
	dst[10] = static_cast<float>(grid_->cell_count());
	dst[11] = static_cast<float>(grid_->edge_count());
	return out;
}

} // namespace godot

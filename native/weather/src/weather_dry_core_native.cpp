#include "weather_dry_core_native.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace godot {

using asterra::weather::GeodesicVoronoiGrid;
using asterra::weather::SphericalLatLonSampler;
using asterra::weather::Vec3d;
using asterra::weather::VoronoiDryCore;
using asterra::weather::VoronoiMoistThermodynamics;
using asterra::weather::dot;

namespace {
constexpr double PI = 3.141592653589793238462643383279502884;

bool finite_positive(double x) {
	return std::isfinite(x) && x > 0.0;
}
} // namespace

void WeatherDryCoreNative::_bind_methods() {
	ClassDB::bind_method(D_METHOD("initialize", "frequency", "surface_pressure_pa", "temperature_k"),
		&WeatherDryCoreNative::initialize, DEFVAL(32), DEFVAL(110000.0), DEFVAL(288.0));
	ClassDB::bind_method(D_METHOD("step", "requested_dt_s", "target_cfl"),
		&WeatherDryCoreNative::step, DEFVAL(0.28));
	ClassDB::bind_method(D_METHOD("reset_isothermal", "surface_pressure_pa", "temperature_k"),
		&WeatherDryCoreNative::reset_isothermal, DEFVAL(110000.0), DEFVAL(288.0));
	ClassDB::bind_method(D_METHOD("reset_terrain_balanced_isothermal", "reference_surface_pressure_pa", "temperature_k"),
		&WeatherDryCoreNative::reset_terrain_balanced_isothermal,
		DEFVAL(110000.0), DEFVAL(288.0));
	ClassDB::bind_method(D_METHOD("initialize_moisture", "relative_humidity", "perform_saturation_adjustment"),
		&WeatherDryCoreNative::initialize_moisture, DEFVAL(0.65), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("disable_moisture", "clear_water"),
		&WeatherDryCoreNative::disable_moisture, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("saturation_adjust_moisture"),
		&WeatherDryCoreNative::saturation_adjust_moisture);
	ClassDB::bind_method(D_METHOD("is_moisture_enabled"),
		&WeatherDryCoreNative::is_moisture_enabled);
	ClassDB::bind_method(D_METHOD("set_surface_height_cells", "height_m"),
		&WeatherDryCoreNative::set_surface_height_cells);
	ClassDB::bind_method(D_METHOD("set_surface_height_map", "height_m", "width", "height"),
		&WeatherDryCoreNative::set_surface_height_map);
	ClassDB::bind_method(D_METHOD("clear_surface_height"),
		&WeatherDryCoreNative::clear_surface_height);
	ClassDB::bind_method(D_METHOD("get_surface_height_cells"),
		&WeatherDryCoreNative::get_surface_height_cells);
	ClassDB::bind_method(D_METHOD("add_pressure_perturbation", "center_direction", "fractional_amplitude", "angular_radius_rad"),
		&WeatherDryCoreNative::add_pressure_perturbation);
	ClassDB::bind_method(D_METHOD("get_global_dry_rgba", "layer"),
		&WeatherDryCoreNative::get_global_dry_rgba, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("get_global_moist_rgba", "layer"),
		&WeatherDryCoreNative::get_global_moist_rgba, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("get_runtime_diagnostics"),
		&WeatherDryCoreNative::get_runtime_diagnostics);
	ClassDB::bind_method(D_METHOD("get_global_budget_diagnostics"),
		&WeatherDryCoreNative::get_global_budget_diagnostics);
	ClassDB::bind_method(D_METHOD("get_moisture_diagnostics"),
		&WeatherDryCoreNative::get_moisture_diagnostics);
	ClassDB::bind_method(D_METHOD("get_frequency"), &WeatherDryCoreNative::get_frequency);
	ClassDB::bind_method(D_METHOD("get_cell_count"), &WeatherDryCoreNative::get_cell_count);
	ClassDB::bind_method(D_METHOD("get_edge_count"), &WeatherDryCoreNative::get_edge_count);
	ClassDB::bind_method(D_METHOD("get_layer_count"), &WeatherDryCoreNative::get_layer_count);
	ClassDB::bind_method(D_METHOD("get_display_width"), &WeatherDryCoreNative::get_display_width);
	ClassDB::bind_method(D_METHOD("get_display_height"), &WeatherDryCoreNative::get_display_height);
	ClassDB::bind_method(D_METHOD("get_simulation_seconds"), &WeatherDryCoreNative::get_simulation_seconds);
	ClassDB::bind_method(D_METHOD("get_top_pressure_pa"), &WeatherDryCoreNative::get_top_pressure_pa);
	ClassDB::bind_method(D_METHOD("get_layer_height_m", "layer"), &WeatherDryCoreNative::get_layer_height_m);
}

Vec3d WeatherDryCoreNative::to_vec3d(const Vector3 &v) {
	return {static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z)};
}

void WeatherDryCoreNative::clear_moisture_state() {
	moisture_enabled_ = false;
	last_moist_adjustment_ = {};
	initial_total_water_kg_ = 0.0;
}

void WeatherDryCoreNative::initialize(int p_frequency, double p_surface_pressure_pa,
		double p_temperature_k) {
	if (p_frequency < 1 || p_frequency > 128) {
		UtilityFunctions::push_error("WeatherDryCoreNative frequency must be in [1, 128]");
		return;
	}
	if (!(p_surface_pressure_pa > TOP_PRESSURE_PA) || !std::isfinite(p_surface_pressure_pa)
			|| !(p_temperature_k > 150.0) || !std::isfinite(p_temperature_k)) {
		UtilityFunctions::push_error("WeatherDryCoreNative initial pressure/temperature is invalid");
		return;
	}

	try {
		auto new_grid = std::make_unique<GeodesicVoronoiGrid>(p_frequency, PLANET_RADIUS_M);
		auto new_dynamics = std::make_unique<VoronoiDryCore>(
			*new_grid, GRAVITY_MPS2, 8000.0, TOP_PRESSURE_PA,
			ROTATION_RATE_RAD_S, Vec3d{0.0, 1.0, 0.0});
		auto new_state = new_dynamics->make_isothermal_reference(
			p_surface_pressure_pa, p_temperature_k);

		grid_ = std::move(new_grid);
		dynamics_ = std::move(new_dynamics);
		state_ = std::move(new_state);
		surface_height_m_.assign(static_cast<size_t>(grid_->cell_count()), 0.0);
		frequency_ = p_frequency;
		simulation_seconds_ = 0.0;
		rejected_steps_total_ = 0;
		last_step_ = {};
		clear_moisture_state();
		rebuild_display_lookup();
		reset_budget_baseline();
		refresh_state_extrema();
	} catch (const std::exception &e) {
		grid_.reset();
		dynamics_.reset();
		state_ = {};
		display_cell_lookup_.clear();
		surface_height_m_.clear();
		frequency_ = 0;
		clear_moisture_state();
		UtilityFunctions::push_error(String("WeatherDryCoreNative initialization failed: ") + e.what());
	}
}

double WeatherDryCoreNative::step(double requested_dt_s, double target_cfl) {
	if (!ready()) {
		UtilityFunctions::push_error("WeatherDryCoreNative.step called before initialize");
		return 0.0;
	}
	if (!finite_positive(requested_dt_s) || !finite_positive(target_cfl) || target_cfl > 0.45) {
		UtilityFunctions::push_error("WeatherDryCoreNative step requires dt > 0 and target_cfl in (0,0.45]");
		return 0.0;
	}

	const VoronoiDryCore::State original = state_;
	const auto previous_moist = last_moist_adjustment_;
	try {
		last_step_ = dynamics_->step(state_, requested_dt_s, target_cfl, 10);
		if (!(last_step_.accepted_dt_s > 0.0)) {
			rejected_steps_total_ += last_step_.rejected_steps;
			return 0.0;
		}

		if (moisture_enabled_) {
			try {
				VoronoiMoistThermodynamics moist(dynamics_->transport());
				last_moist_adjustment_ = moist.saturation_adjust(state_);
			} catch (const std::exception &e) {
				state_ = original;
				last_moist_adjustment_ = previous_moist;
				last_step_.accepted_dt_s = 0.0;
				last_step_.rejected_steps += 1;
				rejected_steps_total_ += last_step_.rejected_steps;
				refresh_state_extrema();
				UtilityFunctions::push_error(String("WeatherDryCoreNative moist source step rolled back: ") + e.what());
				return 0.0;
			}
		}

		rejected_steps_total_ += last_step_.rejected_steps;
		simulation_seconds_ += last_step_.accepted_dt_s;
		refresh_state_extrema();
		return last_step_.accepted_dt_s;
	} catch (const std::exception &e) {
		state_ = original;
		last_moist_adjustment_ = previous_moist;
		UtilityFunctions::push_error(String("WeatherDryCoreNative step failed: ") + e.what());
		return 0.0;
	}
}

void WeatherDryCoreNative::reset_isothermal(double surface_pressure_pa,
		double temperature_k) {
	if (!ready()) {
		UtilityFunctions::push_error("WeatherDryCoreNative.reset_isothermal called before initialize");
		return;
	}
	if (!(surface_pressure_pa > TOP_PRESSURE_PA) || !std::isfinite(surface_pressure_pa)
			|| !(temperature_k > 150.0) || !std::isfinite(temperature_k)) {
		UtilityFunctions::push_error("WeatherDryCoreNative reset pressure/temperature is invalid");
		return;
	}
	try {
		state_ = dynamics_->make_isothermal_reference(surface_pressure_pa, temperature_k);
		simulation_seconds_ = 0.0;
		rejected_steps_total_ = 0;
		last_step_ = {};
		clear_moisture_state();
		reset_budget_baseline();
		refresh_state_extrema();
	} catch (const std::exception &e) {
		UtilityFunctions::push_error(String("WeatherDryCoreNative reset failed: ") + e.what());
	}
}

void WeatherDryCoreNative::reset_terrain_balanced_isothermal(
		double reference_surface_pressure_pa, double temperature_k) {
	if (!ready()) {
		UtilityFunctions::push_error("WeatherDryCoreNative.reset_terrain_balanced_isothermal called before initialize");
		return;
	}
	if (!(reference_surface_pressure_pa > TOP_PRESSURE_PA)
			|| !std::isfinite(reference_surface_pressure_pa)
			|| !(temperature_k > 150.0) || !std::isfinite(temperature_k)) {
		UtilityFunctions::push_error("WeatherDryCoreNative terrain-balanced reset pressure/temperature is invalid");
		return;
	}
	try {
		state_ = dynamics_->make_isothermal_terrain_balanced_reference(
			reference_surface_pressure_pa, temperature_k);
		simulation_seconds_ = 0.0;
		rejected_steps_total_ = 0;
		last_step_ = {};
		clear_moisture_state();
		reset_budget_baseline();
		refresh_state_extrema();
	} catch (const std::exception &e) {
		UtilityFunctions::push_error(String("WeatherDryCoreNative terrain-balanced reset failed: ") + e.what());
	}
}

bool WeatherDryCoreNative::initialize_moisture(double relative_humidity,
		bool perform_saturation_adjustment) {
	if (!ready()) {
		UtilityFunctions::push_error("WeatherDryCoreNative.initialize_moisture called before initialize");
		return false;
	}
	if (!(relative_humidity >= 0.0) || !std::isfinite(relative_humidity)) {
		UtilityFunctions::push_error("WeatherDryCoreNative moisture RH must be finite and non-negative");
		return false;
	}
	const VoronoiDryCore::State original = state_;
	try {
		VoronoiMoistThermodynamics moist(dynamics_->transport());
		moist.initialize_uniform_relative_humidity(state_, relative_humidity);
		last_moist_adjustment_ = {};
		if (perform_saturation_adjustment) {
			last_moist_adjustment_ = moist.saturation_adjust(state_);
		}
		moisture_enabled_ = true;
		reset_moisture_baseline();
		reset_budget_baseline();
		refresh_state_extrema();
		return true;
	} catch (const std::exception &e) {
		state_ = original;
		clear_moisture_state();
		UtilityFunctions::push_error(String("WeatherDryCoreNative moisture initialization failed: ") + e.what());
		return false;
	}
}

void WeatherDryCoreNative::disable_moisture(bool clear_water) {
	if (!ready()) {
		UtilityFunctions::push_error("WeatherDryCoreNative.disable_moisture called before initialize");
		return;
	}
	if (clear_water && state_.tracer_mass_kg_m2.size() >= 3) {
		for (size_t tracer = 0; tracer < 3; ++tracer) {
			std::fill(state_.tracer_mass_kg_m2[tracer].begin(),
				state_.tracer_mass_kg_m2[tracer].end(), 0.0);
		}
	}
	moisture_enabled_ = false;
	last_moist_adjustment_ = {};
	initial_total_water_kg_ = clear_water ? 0.0 : current_total_water_kg();
}

bool WeatherDryCoreNative::saturation_adjust_moisture() {
	if (!ready() || !moisture_enabled_) {
		UtilityFunctions::push_error("WeatherDryCoreNative.saturation_adjust_moisture requires enabled moisture");
		return false;
	}
	const VoronoiDryCore::State original = state_;
	const auto previous = last_moist_adjustment_;
	try {
		VoronoiMoistThermodynamics moist(dynamics_->transport());
		last_moist_adjustment_ = moist.saturation_adjust(state_);
		refresh_state_extrema();
		return true;
	} catch (const std::exception &e) {
		state_ = original;
		last_moist_adjustment_ = previous;
		UtilityFunctions::push_error(String("WeatherDryCoreNative manual saturation adjustment rolled back: ") + e.what());
		return false;
	}
}

void WeatherDryCoreNative::on_static_surface_changed() {
	last_step_ = {};
	reset_budget_baseline();
	refresh_state_extrema();
}

bool WeatherDryCoreNative::set_surface_height_cells(const PackedFloat32Array &height_m) {
	if (!ready()) {
		UtilityFunctions::push_error("WeatherDryCoreNative.set_surface_height_cells called before initialize");
		return false;
	}
	if (height_m.size() != grid_->cell_count()) {
		UtilityFunctions::push_error("WeatherDryCoreNative surface-height cell array must match get_cell_count()");
		return false;
	}
	std::vector<double> candidate(static_cast<size_t>(grid_->cell_count()));
	const float *src = height_m.ptr();
	for (int c = 0; c < grid_->cell_count(); ++c) {
		const double h = static_cast<double>(src[c]);
		if (!std::isfinite(h)) {
			UtilityFunctions::push_error("WeatherDryCoreNative surface-height array contains a non-finite value");
			return false;
		}
		candidate[static_cast<size_t>(c)] = h;
	}
	try {
		dynamics_->set_surface_height_m(candidate);
		surface_height_m_ = std::move(candidate);
		on_static_surface_changed();
		return true;
	} catch (const std::exception &e) {
		UtilityFunctions::push_error(String("WeatherDryCoreNative surface-height upload failed: ") + e.what());
		return false;
	}
}

bool WeatherDryCoreNative::set_surface_height_map(const PackedFloat32Array &height_m,
		int width, int height) {
	if (!ready()) {
		UtilityFunctions::push_error("WeatherDryCoreNative.set_surface_height_map called before initialize");
		return false;
	}
	if (width < 2 || height < 2) {
		UtilityFunctions::push_error("WeatherDryCoreNative surface-height map dimensions must both be >= 2");
		return false;
	}
	const int64_t expected = static_cast<int64_t>(width) * static_cast<int64_t>(height);
	if (expected != static_cast<int64_t>(height_m.size())) {
		UtilityFunctions::push_error("WeatherDryCoreNative surface-height map size does not match width*height");
		return false;
	}

	std::vector<double> raster(static_cast<size_t>(expected));
	const float *src = height_m.ptr();
	for (int64_t i = 0; i < expected; ++i) {
		const double h = static_cast<double>(src[i]);
		if (!std::isfinite(h)) {
			UtilityFunctions::push_error("WeatherDryCoreNative surface-height map contains a non-finite value");
			return false;
		}
		raster[static_cast<size_t>(i)] = h;
	}
	try {
		auto candidate = SphericalLatLonSampler::sample_to_voronoi_cells(
			*grid_, raster, width, height);
		dynamics_->set_surface_height_m(candidate);
		surface_height_m_ = std::move(candidate);
		on_static_surface_changed();
		return true;
	} catch (const std::exception &e) {
		UtilityFunctions::push_error(String("WeatherDryCoreNative surface-height map sampling failed: ") + e.what());
		return false;
	}
}

void WeatherDryCoreNative::clear_surface_height() {
	if (!ready()) {
		UtilityFunctions::push_error("WeatherDryCoreNative.clear_surface_height called before initialize");
		return;
	}
	try {
		surface_height_m_.assign(static_cast<size_t>(grid_->cell_count()), 0.0);
		dynamics_->set_surface_height_m(surface_height_m_);
		on_static_surface_changed();
	} catch (const std::exception &e) {
		UtilityFunctions::push_error(String("WeatherDryCoreNative clear surface height failed: ") + e.what());
	}
}

PackedFloat32Array WeatherDryCoreNative::get_surface_height_cells() const {
	PackedFloat32Array out;
	if (!ready() || surface_height_m_.size() != static_cast<size_t>(grid_->cell_count())) return out;
	out.resize(grid_->cell_count());
	float *dst = out.ptrw();
	for (int c = 0; c < grid_->cell_count(); ++c) {
		dst[c] = static_cast<float>(surface_height_m_[static_cast<size_t>(c)]);
	}
	return out;
}

bool WeatherDryCoreNative::add_pressure_perturbation(const Vector3 &center_direction,
		double fractional_amplitude, double angular_radius_rad) {
	if (!ready()) {
		UtilityFunctions::push_error("WeatherDryCoreNative.add_pressure_perturbation called before initialize");
		return false;
	}
	if (!std::isfinite(fractional_amplitude) || std::abs(fractional_amplitude) > 0.5
			|| !finite_positive(angular_radius_rad) || angular_radius_rad > PI) {
		UtilityFunctions::push_error("WeatherDryCoreNative pressure perturbation parameters are invalid");
		return false;
	}
	const Vec3d raw = to_vec3d(center_direction);
	const double n2 = dot(raw, raw);
	if (!(n2 > 1.0e-24) || !std::isfinite(n2)) {
		UtilityFunctions::push_error("WeatherDryCoreNative perturbation center must be finite and non-zero");
		return false;
	}
	const Vec3d center = raw / std::sqrt(n2);
	std::vector<double> factor(static_cast<size_t>(grid_->cell_count()), 1.0);
	for (int c = 0; c < grid_->cell_count(); ++c) {
		const double angle = std::acos(std::clamp(dot(center, grid_->cell(c).center), -1.0, 1.0));
		if (angle >= angular_radius_rad) continue;
		const double bell = 0.5 * (1.0 + std::cos(PI * angle / angular_radius_rad));
		factor[static_cast<size_t>(c)] += fractional_amplitude * bell;
		if (!(factor[static_cast<size_t>(c)] > 0.0) || !std::isfinite(factor[static_cast<size_t>(c)])) {
			UtilityFunctions::push_error("WeatherDryCoreNative perturbation would make dry mass non-positive");
			return false;
		}
	}
	for (int k = 0; k < VoronoiDryCore::LEVELS; ++k) {
		for (int c = 0; c < grid_->cell_count(); ++c) {
			const size_t i = static_cast<size_t>(k * grid_->cell_count() + c);
			const double f = factor[static_cast<size_t>(c)];
			state_.layer_mass_kg_m2[i] *= f;
			state_.theta_mass_kg_k_m2[i] *= f;
			for (auto &tracer : state_.tracer_mass_kg_m2) tracer[i] *= f;
		}
	}
	last_step_ = {};
	reset_budget_baseline();
	if (moisture_enabled_) reset_moisture_baseline();
	refresh_state_extrema();
	return true;
}

int WeatherDryCoreNative::nearest_cell_hill_climb(const Vec3d &direction, int seed) const {
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

void WeatherDryCoreNative::rebuild_display_lookup() {
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

void WeatherDryCoreNative::reset_budget_baseline() {
	if (!ready()) {
		initial_dry_mass_kg_ = 0.0;
		initial_theta_mass_kg_k_ = 0.0;
		initial_dry_energy_j_ = 0.0;
		initial_absolute_aam_kg_m2_s_ = 0.0;
		return;
	}
	initial_dry_mass_kg_ = dynamics_->total_dry_mass_kg(state_);
	initial_theta_mass_kg_k_ = dynamics_->total_theta_mass_kg_k(state_);
	initial_dry_energy_j_ = dynamics_->total_dry_energy_j(state_);
	initial_absolute_aam_kg_m2_s_ =
		dynamics_->total_absolute_axial_angular_momentum_kg_m2_s(state_);
}

double WeatherDryCoreNative::current_total_water_kg() const {
	if (!ready() || state_.tracer_mass_kg_m2.size() < 3) return 0.0;
	return dynamics_->total_tracer_mass_kg(state_, 0)
		+ dynamics_->total_tracer_mass_kg(state_, 1)
		+ dynamics_->total_tracer_mass_kg(state_, 2);
}

void WeatherDryCoreNative::reset_moisture_baseline() {
	initial_total_water_kg_ = current_total_water_kg();
}

void WeatherDryCoreNative::refresh_state_extrema() {
	last_step_.min_layer_mass_kg_m2 = std::numeric_limits<double>::infinity();
	last_step_.min_potential_temperature_k = std::numeric_limits<double>::infinity();
	last_step_.max_speed_mps = 0.0;
	for (size_t i = 0; i < state_.layer_mass_kg_m2.size(); ++i) {
		const double mass = state_.layer_mass_kg_m2[i];
		last_step_.min_layer_mass_kg_m2 = std::min(last_step_.min_layer_mass_kg_m2, mass);
		last_step_.min_potential_temperature_k = std::min(last_step_.min_potential_temperature_k,
			state_.theta_mass_kg_k_m2[i] / mass);
	}
	for (double u : state_.edge_normal_mps) {
		last_step_.max_speed_mps = std::max(last_step_.max_speed_mps, std::abs(u));
	}
	if (ready()) {
		last_step_.max_coordinate_mass_fraction_error =
			dynamics_->max_coordinate_mass_fraction_error(state_);
	}
}

PackedFloat32Array WeatherDryCoreNative::get_global_dry_rgba(int layer) const {
	PackedFloat32Array out;
	if (!ready() || layer < 0 || layer >= VoronoiDryCore::LEVELS
			|| display_cell_lookup_.size() != static_cast<size_t>(DISPLAY_W * DISPLAY_H)) return out;

	const auto hydro = dynamics_->transport().diagnose_hydrostatic(state_);
	std::vector<double> cell_speed(static_cast<size_t>(grid_->cell_count()), 0.0);
	for (int c = 0; c < grid_->cell_count(); ++c) {
		long double sum = 0.0L;
		for (int e : grid_->cell(c).edges) {
			const double u = state_.edge_normal_mps[static_cast<size_t>(layer * grid_->edge_count() + e)];
			sum += static_cast<long double>(grid_->edge(e).edge_area_m2)
				* static_cast<long double>(u * u);
		}
		const double kinetic = static_cast<double>(
			sum / (4.0L * static_cast<long double>(grid_->cell(c).area_m2)));
		cell_speed[static_cast<size_t>(c)] = std::sqrt(std::max(0.0, 2.0 * kinetic));
	}

	out.resize(DISPLAY_W * DISPLAY_H * 4);
	float *dst = out.ptrw();
	for (int p = 0; p < DISPLAY_W * DISPLAY_H; ++p) {
		const int c = display_cell_lookup_[static_cast<size_t>(p)];
		const size_t i = static_cast<size_t>(layer * grid_->cell_count() + c);
		dst[4 * p + 0] = static_cast<float>(hydro.temperature_k[i]);
		dst[4 * p + 1] = static_cast<float>(cell_speed[static_cast<size_t>(c)]);
		dst[4 * p + 2] = static_cast<float>(hydro.surface_pressure_pa[static_cast<size_t>(c)]);
		dst[4 * p + 3] = static_cast<float>(state_.layer_mass_kg_m2[i]);
	}
	return out;
}

PackedFloat32Array WeatherDryCoreNative::get_global_moist_rgba(int layer) const {
	PackedFloat32Array out;
	if (!ready() || !moisture_enabled_ || layer < 0 || layer >= VoronoiDryCore::LEVELS
			|| state_.tracer_mass_kg_m2.size() < 3
			|| display_cell_lookup_.size() != static_cast<size_t>(DISPLAY_W * DISPLAY_H)) return out;

	const auto hydro = dynamics_->transport().diagnose_hydrostatic(state_);
	out.resize(DISPLAY_W * DISPLAY_H * 4);
	float *dst = out.ptrw();
	for (int p = 0; p < DISPLAY_W * DISPLAY_H; ++p) {
		const int c = display_cell_lookup_[static_cast<size_t>(p)];
		const size_t i = static_cast<size_t>(layer * grid_->cell_count() + c);
		const double dry = state_.layer_mass_kg_m2[i];
		const double qv = state_.tracer_mass_kg_m2[0][i] / dry;
		const double ql = state_.tracer_mass_kg_m2[1][i] / dry;
		const double qi = state_.tracer_mass_kg_m2[2][i] / dry;
		const double es = VoronoiMoistThermodynamics::saturation_vapor_pressure_pa(
			hydro.temperature_k[i]);
		double rh = 0.0;
		if (hydro.layer_pressure_pa[i] > es) {
			const double qsat = VoronoiMoistThermodynamics::EPSILON * es
				/ (hydro.layer_pressure_pa[i] - es);
			if (qsat > 0.0) rh = qv / qsat;
		}
		dst[4 * p + 0] = static_cast<float>(qv);
		dst[4 * p + 1] = static_cast<float>(ql);
		dst[4 * p + 2] = static_cast<float>(qi);
		dst[4 * p + 3] = static_cast<float>(rh);
	}
	return out;
}

PackedFloat32Array WeatherDryCoreNative::get_runtime_diagnostics() const {
	PackedFloat32Array out;
	if (!ready()) return out;
	out.resize(20);
	float *dst = out.ptrw();
	const double mass = dynamics_->total_dry_mass_kg(state_);
	const double theta_mass = dynamics_->total_theta_mass_kg_k(state_);
	const double mass_drift = initial_dry_mass_kg_ != 0.0
		? (mass - initial_dry_mass_kg_) / initial_dry_mass_kg_ : 0.0;
	const double theta_drift = initial_theta_mass_kg_k_ != 0.0
		? (theta_mass - initial_theta_mass_kg_k_) / initial_theta_mass_kg_k_ : 0.0;

	dst[0] = static_cast<float>(simulation_seconds_);
	dst[1] = static_cast<float>(last_step_.requested_dt_s);
	dst[2] = static_cast<float>(last_step_.accepted_dt_s);
	dst[3] = static_cast<float>(last_step_.max_courant);
	dst[4] = static_cast<float>(rejected_steps_total_);
	dst[5] = static_cast<float>(mass_drift);
	dst[6] = static_cast<float>(theta_drift);
	dst[7] = static_cast<float>(last_step_.min_layer_mass_kg_m2);
	dst[8] = static_cast<float>(last_step_.min_potential_temperature_k);
	dst[9] = static_cast<float>(last_step_.max_speed_mps);
	dst[10] = static_cast<float>(last_step_.max_pressure_acceleration_mps2);
	dst[11] = static_cast<float>(grid_->cell_count());
	dst[12] = static_cast<float>(grid_->edge_count());
	dst[13] = static_cast<float>(VoronoiDryCore::LEVELS);
	dst[14] = static_cast<float>(TOP_PRESSURE_PA);
	dst[15] = static_cast<float>(last_step_.max_coordinate_mass_fraction_error);
	dst[16] = static_cast<float>(last_step_.max_coordinate_column_mass_error);
	dst[17] = static_cast<float>(last_step_.max_coordinate_column_theta_mass_error);
	dst[18] = static_cast<float>(last_step_.max_coordinate_edge_momentum_error);
	dst[19] = last_step_.coordinate_remap_applied ? 1.0f : 0.0f;
	return out;
}

PackedFloat64Array WeatherDryCoreNative::get_global_budget_diagnostics() const {
	PackedFloat64Array out;
	if (!ready()) return out;
	const double mass = dynamics_->total_dry_mass_kg(state_);
	const double theta_mass = dynamics_->total_theta_mass_kg_k(state_);
	const double energy = dynamics_->total_dry_energy_j(state_);
	const double relative_aam =
		dynamics_->total_relative_axial_angular_momentum_kg_m2_s(state_);
	const double absolute_aam =
		dynamics_->total_absolute_axial_angular_momentum_kg_m2_s(state_);
	const double energy_drift = initial_dry_energy_j_ != 0.0
		? (energy - initial_dry_energy_j_) / std::abs(initial_dry_energy_j_) : 0.0;
	const double aam_drift = initial_absolute_aam_kg_m2_s_ != 0.0
		? (absolute_aam - initial_absolute_aam_kg_m2_s_)
			/ std::abs(initial_absolute_aam_kg_m2_s_) : 0.0;

	out.resize(10);
	double *dst = out.ptrw();
	dst[0] = mass;
	dst[1] = theta_mass;
	dst[2] = energy;
	dst[3] = energy_drift;
	dst[4] = relative_aam;
	dst[5] = absolute_aam;
	dst[6] = aam_drift;
	dst[7] = dynamics_->max_coordinate_mass_fraction_error(state_);
	dst[8] = initial_dry_energy_j_;
	dst[9] = initial_absolute_aam_kg_m2_s_;
	return out;
}

PackedFloat64Array WeatherDryCoreNative::get_moisture_diagnostics() const {
	PackedFloat64Array out;
	if (!ready()) return out;
	out.resize(15);
	double *dst = out.ptrw();
	const double water = current_total_water_kg();
	const double water_drift = initial_total_water_kg_ != 0.0
		? (water - initial_total_water_kg_) / initial_total_water_kg_ : 0.0;
	dst[0] = moisture_enabled_ ? 1.0 : 0.0;
	dst[1] = initial_total_water_kg_;
	dst[2] = water;
	dst[3] = water_drift;
	dst[4] = last_moist_adjustment_.relative_total_water_error;
	dst[5] = last_moist_adjustment_.max_relative_cell_water_error;
	dst[6] = last_moist_adjustment_.max_specific_enthalpy_error_j_kg;
	dst[7] = last_moist_adjustment_.max_relative_humidity_before;
	dst[8] = last_moist_adjustment_.max_relative_humidity_after;
	dst[9] = last_moist_adjustment_.max_abs_temperature_change_k;
	dst[10] = last_moist_adjustment_.condensed_water_kg;
	dst[11] = last_moist_adjustment_.evaporated_water_kg;
	dst[12] = last_moist_adjustment_.min_temperature_k;
	dst[13] = last_moist_adjustment_.max_temperature_k;
	dst[14] = static_cast<double>(last_moist_adjustment_.saturated_cell_count);
	return out;
}

} // namespace godot

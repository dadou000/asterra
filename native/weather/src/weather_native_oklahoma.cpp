// Oklahoma/Great-Plains-style tuning layer for Asterra weather.
//
// The large AVX2 core remains in weather_native.cpp. This translation unit is
// the only one compiled by CMake and includes that proven core under a handful
// of *_original names, then wraps the cloud/convection entry points. This keeps
// the numerical solver intact while making the sparse-cloud / capped-severe
// regime explicit and reviewable in one small file.

#include "weather_native.h"

#define _bind_methods _bind_methods_original
#define horizontal_pass horizontal_pass_original
#define vertical_pass vertical_pass_original
#define step_global step_global_original
#define step_local step_local_original
#define get_global_weather_rgba get_global_weather_rgba_original
#define get_local_weather_rgba get_local_weather_rgba_original
#include "weather_native.cpp"
#undef get_local_weather_rgba
#undef get_global_weather_rgba
#undef step_local
#undef step_global
#undef vertical_pass
#undef horizontal_pass
#undef _bind_methods

namespace godot {

static inline float severe_smooth01(float x) {
	x = std::clamp(x, 0.0f, 1.0f);
	return x * x * (3.0f - 2.0f * x);
}

static inline int neighbour_cell(const WeatherNative::Atmosphere &a, bool is_global,
		int x, int y) {
	if (is_global) {
		x %= a.width;
		if (x < 0) x += a.width;
		y = std::max(0, std::min(a.height - 1, y));
		return x + y * a.width;
	}
	x = std::max(0, std::min(a.width - 1, x));
	y = std::max(0, std::min(a.height - 1, y));
	return x + y * a.width;
}

void WeatherNative::_bind_methods() {
	ClassDB::bind_method(D_METHOD("initialize", "seed"), &WeatherNative::initialize);
	ClassDB::bind_method(D_METHOD("step_global", "dt"), &WeatherNative::step_global);
	ClassDB::bind_method(D_METHOD("set_local_center", "center_dir"), &WeatherNative::set_local_center);
	ClassDB::bind_method(D_METHOD("step_local", "dt"), &WeatherNative::step_local);
	ClassDB::bind_method(D_METHOD("reset_local_from_global"), &WeatherNative::reset_local_from_global);
	ClassDB::bind_method(D_METHOD("get_global_weather_rgba"), &WeatherNative::get_global_weather_rgba);
	ClassDB::bind_method(D_METHOD("get_local_weather_rgba"), &WeatherNative::get_local_weather_rgba);
	ClassDB::bind_method(D_METHOD("get_global_diagnostics_rgba"), &WeatherNative::get_global_diagnostics_rgba);
	ClassDB::bind_method(D_METHOD("get_local_diagnostics_rgba"), &WeatherNative::get_local_diagnostics_rgba);
	ClassDB::bind_method(D_METHOD("get_global_convective_rgba"), &WeatherNative::get_global_convective_rgba);
	ClassDB::bind_method(D_METHOD("get_local_convective_rgba"), &WeatherNative::get_local_convective_rgba);
	ClassDB::bind_method(D_METHOD("get_tropical_core_diagnostics"), &WeatherNative::get_tropical_core_diagnostics);
	ClassDB::bind_method(D_METHOD("set_tuning_weight", "name", "value"), &WeatherNative::set_tuning_weight);
	ClassDB::bind_method(D_METHOD("get_tuning_weight", "name"), &WeatherNative::get_tuning_weight);
	ClassDB::bind_method(D_METHOD("reset_tuning_weights"), &WeatherNative::reset_tuning_weights);
	ClassDB::bind_method(D_METHOD("set_layer_weight", "layer", "value"), &WeatherNative::set_layer_weight);
	ClassDB::bind_method(D_METHOD("get_layer_weights"), &WeatherNative::get_layer_weights);
	ClassDB::bind_method(D_METHOD("get_layer_count"), &WeatherNative::get_layer_count);
	ClassDB::bind_method(D_METHOD("get_global_width"), &WeatherNative::get_global_width);
	ClassDB::bind_method(D_METHOD("get_global_height"), &WeatherNative::get_global_height);
	ClassDB::bind_method(D_METHOD("get_layer_height_m", "layer"), &WeatherNative::get_layer_height_m);
	ClassDB::bind_method(D_METHOD("get_local_center"), &WeatherNative::get_local_center);
	ClassDB::bind_method(D_METHOD("get_local_east"), &WeatherNative::get_local_east);
	ClassDB::bind_method(D_METHOD("get_local_north"), &WeatherNative::get_local_north);
	ClassDB::bind_method(D_METHOD("get_local_span_m"), &WeatherNative::get_local_span_m);
	ClassDB::bind_method(D_METHOD("set_global_surface_fields", "fields"), &WeatherNative::set_global_surface_fields);
	ClassDB::bind_method(D_METHOD("set_local_surface_fields", "fields"), &WeatherNative::set_local_surface_fields);
	ClassDB::bind_method(D_METHOD("set_solar_forcing", "sun_direction_body", "irradiance_w_m2", "angular_radius_rad"), &WeatherNative::set_solar_forcing);
	ClassDB::bind_method(D_METHOD("get_global_surface_rgba"), &WeatherNative::get_global_surface_rgba);
	ClassDB::bind_method(D_METHOD("get_local_surface_rgba"), &WeatherNative::get_local_surface_rgba);
	ClassDB::bind_method(D_METHOD("get_global_products_rgba"), &WeatherNative::get_global_products_rgba);
	ClassDB::bind_method(D_METHOD("get_local_products_rgba"), &WeatherNative::get_local_products_rgba);
}

void WeatherNative::horizontal_pass(Atmosphere &a, bool is_global, float dt) {
	// Run the proven transport/momentum core without its legacy 68%-RH cloud
	// microphysics. That branch was the source of the runaway: it converted broad
	// climatological humidity into condensate every step while the humidity nudger
	// simultaneously refilled vapour. The sparse closure below now owns all cloud
	// phase changes. Keep only a very weak global humidity climatology as a slow
	// large-scale anchor; the coupled surface evaporation is the real water source.
	const float requested_humidity_weight = tuning_weights[HUMIDITY];
	const float requested_cloud_weight = tuning_weights[CLOUD_MICROPHYSICS];
	tuning_weights[HUMIDITY] = requested_humidity_weight * (is_global ? 0.12f : 0.0f);
	tuning_weights[CLOUD_MICROPHYSICS] = 0.0f;
	horizontal_pass_original(a, is_global, dt);
	tuning_weights[HUMIDITY] = requested_humidity_weight;
	tuning_weights[CLOUD_MICROPHYSICS] = requested_cloud_weight;

	if (a.convective_activation.size() != size_t(a.cells)) {
		a.convective_activation.assign(a.cells, 0.0f);
	}
	if (is_global && global_simulation_seconds <= 0.0) {
		std::fill(a.convective_activation.begin(), a.convective_activation.end(), 0.0f);
	}

	const int l0_layer = 0;
	const int l1_layer = nearest_layer_for_height(1700.0f);
	const int l2_layer = nearest_layer_for_height(3300.0f);
	const int l0_off = a.layer_offset(l0_layer);
	const int l1_off = a.layer_offset(l1_layer);
	const int l2_off = a.layer_offset(l2_layer);
	const SurfaceState &surface = is_global ? global_surface : local_surface;
	const bool have_surface = surface_fields_ready && (is_global || local_surface_fields_ready);
	const float rise_tau = is_global ? 900.0f : 420.0f;
	const float decay_tau = 2700.0f;

	// Elevated mixed-layer source. Hot, dry continental surfaces build a warm
	// 1.7-km layer over several hours. Horizontal advection then carries that cap
	// away from the dry source region, allowing a humid boundary layer on the
	// downstream side of a dryline to become highly unstable while still clear.
	if (have_surface) {
		#pragma omp parallel for schedule(static)
		for (int c = 0; c < a.cells; ++c) {
			const float land = 1.0f - std::clamp(surface.water_fraction[c], 0.0f, 1.0f);
			const float dryness = severe_smooth01((0.52f - surface.soil_moisture[c]) / 0.38f);
			const float heat = severe_smooth01((surface.sensible_flux_w_m2[c] - 65.0f) / 285.0f);
			const float solar = severe_smooth01((surface.absorbed_solar_w_m2[c] - 180.0f) / 520.0f);
			const float source = land * dryness * std::max(heat, solar * 0.72f);
			if (source < 0.015f) continue;

			const float q0 = a.nq[l0_off + c];
			const float q1 = a.nq[l1_off + c];
			const float moisture_barrier = std::min(
				1400.0f * std::max(q0 - q1, 0.0f), 10.0f);
			const float target_theta1 = a.ntheta[l0_off + c] + moisture_barrier + 5.0f;
			if (target_theta1 <= a.ntheta[l1_off + c]) continue;
			const float eml_tau = 4.5f * 3600.0f;
			const float relax = 1.0f - std::exp(-source * dt / eml_tau);
			a.ntheta[l1_off + c] = std::clamp(std::lerp(
				a.ntheta[l1_off + c], target_theta1, relax), 220.0f, 430.0f);
		}
	}

	// Capped severe-convection closure. Deep moist instability can remain loaded
	// while the 0.45-1.7 km layer is inhibiting. Convergence, strong surface heat,
	// or a sharp low-level moisture boundary (dryline) can release that reservoir.
	#pragma omp parallel for schedule(static)
	for (int y = 0; y < a.height; ++y) {
		for (int x = 0; x < a.width; ++x) {
			const int c = x + y * a.width;
			const float th0 = a.ntheta[l0_off + c];
			const float th1 = a.ntheta[l1_off + c];
			const float th2 = a.ntheta[l2_off + c];
			const float q0 = a.nq[l0_off + c];
			const float q1 = a.nq[l1_off + c];
			const float q2 = a.nq[l2_off + c];

			const float low_moist_excess = th0 - th1 + 1400.0f * (q0 - q1);
			const float deep_moist_excess = th0 - th2 + 1400.0f * (q0 - q2);
			const float loaded = severe_smooth01((deep_moist_excess - 1.5f) / 14.0f);
			const float cap = severe_smooth01((-low_moist_excess + 1.0f) / 9.0f);
			const float moisture = severe_smooth01((q0 - 0.0105f) / 0.0075f);

			const int ce = neighbour_cell(a, is_global, x + 1, y);
			const int cw = neighbour_cell(a, is_global, x - 1, y);
			const int cn = neighbour_cell(a, is_global, x, y - 1);
			const int cs = neighbour_cell(a, is_global, x, y + 1);
			float q_contrast = 0.0f;
			q_contrast = std::max(q_contrast, std::abs(q0 - a.nq[l0_off + ce]));
			q_contrast = std::max(q_contrast, std::abs(q0 - a.nq[l0_off + cw]));
			q_contrast = std::max(q_contrast, std::abs(q0 - a.nq[l0_off + cn]));
			q_contrast = std::max(q_contrast, std::abs(q0 - a.nq[l0_off + cs]));
			const float dryline_start = is_global ? 0.00055f : 0.00030f;
			const float dryline_width = is_global ? 0.00155f : 0.00100f;
			const float dryline = severe_smooth01((q_contrast - dryline_start) / dryline_width);

			const float convergence_scale = is_global ? 7.0e-5f : 2.0e-4f;
			const float convergence = severe_smooth01(
				(-a.divergence[l0_off + c] - 0.12f * convergence_scale) / convergence_scale);
			float heat = 0.0f;
			if (have_surface) {
				heat = severe_smooth01((surface.sensible_flux_w_m2[c] - 70.0f) / 260.0f);
			}
			const float previous = std::clamp(a.convective_activation[c], 0.0f, 1.0f);
			const float precip_memory = severe_smooth01((a.nprecip[c] - 0.025f) / 0.20f);
			const float boundary_trigger = std::max(convergence,
				std::max(dryline * std::max(heat, 0.42f), heat * 0.58f));
			const float required_trigger = 0.24f + 0.54f * cap;
			const float release = loaded * moisture
				* severe_smooth01((boundary_trigger - required_trigger) / 0.24f);
			const float sustain = std::max(release,
				std::max(precip_memory * 0.92f, previous * 0.74f));
			const float tau = sustain > previous ? rise_tau : decay_tau;
			const float relax = 1.0f - std::exp(-dt / tau);
			a.convective_activation[c] = std::clamp(
				std::lerp(previous, sustain, relax), 0.0f, 1.0f);
		}
	}

	// Single-owner sparse cloud microphysics. Vapour RH determines whether a new
	// sub-grid cloud fraction may form; existing condensate is not allowed to
	// inflate its own formation RH. Total water is used only to enforce saturated
	// equilibrium and therefore remains conserved through condensation/evaporation.
	const float microphysics_scale = std::clamp(requested_cloud_weight, 0.0f, 2.0f);
	for (int layer = 0; layer < LAYERS; ++layer) {
		const int off = a.layer_offset(layer);
		const float sf = sigma_temperature_factor(layer);
		const float fair_onset_rh = APPROX_HEIGHT_M[layer] <= 2000.0f ? 0.90f : 0.93f;
		const float fair_full_rh = APPROX_HEIGHT_M[layer] <= 2000.0f ? 0.985f : 0.995f;
		#pragma omp parallel for schedule(static)
		for (int c = 0; c < a.cells; ++c) {
			const int i = off + c;
			float qv = std::clamp(a.nq[i], 0.00001f, 0.032f);
			float liquid = std::max(a.nliquid[i], 0.0f);
			float ice = std::max(a.nice[i], 0.0f);
			float cloud_total = liquid + ice;
			float temperature = a.ntheta[i] * sf;
			const float pabs = std::clamp(P0 * SIGMA[layer] + a.npressure[i], 12000.0f, 115000.0f);
			const float saturation = std::max(qsat_scalar(temperature, pabs), 2e-5f);
			const float total_water = qv + cloud_total;
			const float vapor_rh = qv / saturation;

			const float active = a.convective_activation.size() == size_t(a.cells)
				? std::clamp(a.convective_activation[c], 0.0f, 1.0f) : 0.0f;
			const float precip_active = severe_smooth01((a.nprecip[c] - 0.055f) / 0.24f);
			const float storm_keep = std::clamp(std::max(active, precip_active), 0.0f, 1.0f);

			// If total water exceeds saturation, enough condensate must remain to
			// keep vapour at qsat. This is the resolved cloud component.
			const float resolved_target = std::max(total_water - saturation, 0.0f);
			float target_cloud = resolved_target;

			// Fair-weather cloud fraction exists only when *vapour* RH is already
			// close to saturation. A released storm lowers the onset and turns much
			// more of the near-saturated reservoir into optically deep condensate.
			const float onset_rh = std::lerp(fair_onset_rh, 0.80f, storm_keep);
			const float full_rh = std::lerp(fair_full_rh, 0.95f, storm_keep);
			if (vapor_rh > onset_rh) {
				const float partial = severe_smooth01(
					(vapor_rh - onset_rh) / std::max(full_rh - onset_rh, 0.001f));
				const float available = std::max(
					total_water - saturation * onset_rh - resolved_target, 0.0f);
				const float fair_fraction = 0.012f + 0.070f * partial;
				const float storm_fraction = 0.18f + 0.72f * partial;
				const float retained_fraction = std::lerp(fair_fraction, storm_fraction, storm_keep);
				target_cloud += available * retained_fraction;
			}
			target_cloud = std::clamp(target_cloud, 0.0f, std::max(total_water - 0.00001f, 0.0f));

			if (target_cloud > cloud_total) {
				// New cloud forms quickly only when actually supersaturated or when a
				// released convective column is close to saturation.
				const float formation_tau = std::lerp(
					is_global ? 900.0f : 420.0f,
					is_global ? 120.0f : 60.0f,
					storm_keep);
				const float formation_fraction = 1.0f - std::exp(
					-microphysics_scale * dt / std::max(formation_tau, 1.0f));
				const float formed = std::min(
					(target_cloud - cloud_total) * formation_fraction,
					std::max(qv - 0.00001f, 0.0f));
				if (formed > 0.0f) {
					const float ice_frac = std::clamp((268.0f - temperature) / 20.0f, 0.0f, 1.0f);
					qv -= formed;
					liquid += formed * (1.0f - ice_frac);
					ice += formed * ice_frac;
					a.ntheta[i] = std::clamp(
						a.ntheta[i] + formed * (2500.0f / sf), 220.0f, 430.0f);
				}
			} else if (target_cloud < cloud_total) {
				// Fair cloud clears rapidly; anvils/active storm condensate retain a
				// much longer memory. Evaporation returns exactly the removed mass to q.
				const float liquid_tau = std::lerp(APPROX_HEIGHT_M[layer] <= 2000.0f ? 480.0f : 720.0f,
					4800.0f, storm_keep);
				const float ice_tau = std::lerp(1500.0f, 14400.0f, storm_keep);
				const float liquid_share = cloud_total > 1e-9f ? liquid / cloud_total : 0.0f;
				const float effective_tau = std::lerp(ice_tau, liquid_tau, liquid_share);
				const float evap_fraction = 1.0f - std::exp(
					-microphysics_scale * dt / std::max(effective_tau, 1.0f));
				const float evaporated = (cloud_total - target_cloud) * evap_fraction;
				if (evaporated > 0.0f) {
					const float remove_liquid = evaporated * liquid_share;
					liquid = std::max(liquid - remove_liquid, 0.0f);
					ice = std::max(ice - (evaporated - remove_liquid), 0.0f);
					qv = std::min(qv + evaporated, 0.032f);
					a.ntheta[i] = std::clamp(
						a.ntheta[i] - evaporated * (2488.0f / sf), 220.0f, 430.0f);
				}
			}

			// Phase conversion remains prognostic. This is intentionally separate
			// from cloud amount so cold anvils can glaciate without creating water.
			temperature = a.ntheta[i] * sf;
			const float phase_scale = std::max(microphysics_scale, 0.05f);
			const float freeze_strength = std::clamp((260.0f - temperature) / 14.0f, 0.0f, 1.0f);
			const float freeze = liquid * freeze_strength
				* (1.0f - std::exp(-phase_scale * dt / 900.0f));
			liquid -= freeze;
			ice += freeze;
			const float melt_strength = std::clamp((temperature - 273.15f) / 8.0f, 0.0f, 1.0f);
			const float melt = ice * melt_strength
				* (1.0f - std::exp(-phase_scale * dt / 700.0f));
			ice -= melt;
			liquid += melt;
			a.ntheta[i] = std::clamp(
				a.ntheta[i] + (freeze - melt) * (333.0f / sf), 220.0f, 430.0f);

			a.nq[i] = std::clamp(qv, 0.00001f, 0.032f);
			a.nliquid[i] = std::clamp(liquid, 0.0f, 0.012f);
			a.nice[i] = std::clamp(ice, 0.0f, 0.012f);
		}
	}
}

void WeatherNative::vertical_pass(Atmosphere &a, bool is_global, float dt) {
	if (a.convective_activation.size() != size_t(a.cells)) {
		a.convective_activation.assign(a.cells, 0.0f);
	}

	// Let the proven vertical/tropical closure handle background overturning,
	// downdrafts, cold pools and warm-core cyclones, but deliberately weaken the
	// everywhere-at-once moist branch. Released columns receive their energy back
	// below as localized, substantially stronger updrafts.
	const float original_convection = tuning_weights[CONVECTION];
	tuning_weights[CONVECTION] = original_convection * (is_global ? 0.68f : 0.52f);
	vertical_pass_original(a, is_global, dt);
	tuning_weights[CONVECTION] = original_convection;

	const float base_rate = (is_global ? 2.4e-4f : 8.5e-4f) * original_convection * old_six_level_column_scale();
	const float max_extra_mix = is_global ? 0.035f : 0.075f;
	#pragma omp parallel for schedule(static)
	for (int c = 0; c < a.cells; ++c) {
		const float active = std::clamp(a.convective_activation[c], 0.0f, 1.0f);
		if (active < 0.015f) {
			for (int interface_index = 0; interface_index < INTERFACES; ++interface_index) {
				int fi = a.interface_offset(interface_index) + c;
				a.mass_flux[fi] *= 0.32f;
			}
			continue;
		}

		for (int interface_index = 0; interface_index < INTERFACES; ++interface_index) {
			const int lo = interface_index;
			const int hi = interface_index + 1;
			const int lo_i = a.layer_offset(lo) + c;
			const int hi_i = a.layer_offset(hi) + c;
			const int fi = a.interface_offset(interface_index) + c;
			const float depth_factor = std::max(1.0f - 0.52f * std::clamp(APPROX_HEIGHT_M[hi] / 12800.0f, 0.0f, 1.0f), 0.45f);
			const float burst_rate = base_rate * active * depth_factor;
			a.mass_flux[fi] = std::clamp(std::max(a.mass_flux[fi], burst_rate),
				-1.8e-4f, is_global ? 7.0e-4f : 1.5e-3f);
			const float mix = std::clamp(burst_rate * dt * 0.78f, 0.0f, max_extra_mix);
			if (mix <= 0.0f) continue;

			auto upward_mix = [&](std::vector<float> &field, float upper_gain = 0.78f) {
				const float lower = field[lo_i];
				const float upper = field[hi_i];
				const float delta = lower - upper;
				field[lo_i] = lower - mix * delta;
				field[hi_i] = upper + mix * upper_gain * delta;
			};
			upward_mix(a.ntheta, 0.72f);
			upward_mix(a.nq, 0.88f);
			upward_mix(a.nu, 0.62f);
			upward_mix(a.nv, 0.62f);
			upward_mix(a.npressure, 0.58f);

			float loft = std::clamp(mix * (0.75f + 0.45f * active), 0.0f, 0.10f);
			float lifted_liquid = a.nliquid[lo_i] * loft;
			a.nliquid[lo_i] = std::max(a.nliquid[lo_i] - lifted_liquid, 0.0f);
			a.nice[hi_i] = std::clamp(a.nice[hi_i] + lifted_liquid * 0.88f, 0.0f, 0.012f);
		}
	}
}

void WeatherNative::step_global(float dt) {
	dt = std::clamp(dt, 1.0f, 180.0f);
	horizontal_pass(global_atm, true, dt);
	if (surface_fields_ready) surface_pass(global_atm, global_surface, true, dt);
	vertical_pass(global_atm, true, dt);
	filter_global_poles(global_atm);
	center_global_pressure(global_atm.npressure);
	swap_state(global_atm);
	diagnose(global_atm, true);
	global_simulation_seconds += double(dt);
}

void WeatherNative::step_local(float dt) {
	if (!local_initialized) initialize_local();
	dt = std::clamp(dt, 1.0f, 40.0f);
	int substeps = std::max(1, int(std::ceil(dt / 10.0f)));
	float sub_dt = dt / float(substeps);
	for (int substep = 0; substep < substeps; ++substep) {
		horizontal_pass(local_atm, false, sub_dt);
		if (surface_fields_ready) surface_pass(local_atm, local_surface, false, sub_dt);
		vertical_pass(local_atm, false, sub_dt);
		swap_state(local_atm);
		diagnose(local_atm, false);
	}
}

static PackedFloat32Array remap_sparse_severe_weather(PackedFloat32Array values,
		const WeatherNative::Atmosphere &a) {
	if (values.size() != a.cells * 4) return values;
	float *w = values.ptrw();
	#pragma omp parallel for schedule(static)
	for (int c = 0; c < a.cells; ++c) {
		const int o = c * 4;
		const float old_storm = std::clamp(w[o + 1], 0.0f, 1.0f);
		const float active = a.convective_activation.size() == size_t(a.cells)
			? std::clamp(a.convective_activation[c], 0.0f, 1.0f) : 0.0f;
		const float precip_memory = severe_smooth01((w[o + 2] - 0.025f) / 0.22f);
		const float release = std::max(active, precip_memory * 0.78f);
		const float storm = std::clamp(old_storm * (0.10f + 0.90f * release), 0.0f, 1.0f);

		// Presentation is intentionally conservative. The physical condensate is
		// already pruned above; this map must not turn trace condensate back into an
		// overcast sky. Severe cells retain vertical/density intensity through G.
		const float legacy_base = std::clamp(w[o] - old_storm * 0.14f, 0.0f, 0.9995f);
		const float condensate_proxy = -std::log(std::max(1.0f - legacy_base, 5e-4f)) / 4000.0f;
		const float cloud = std::clamp(
			1.0f - std::exp(-condensate_proxy * 600.0f) + storm * 0.020f,
			0.0f, 1.0f);
		w[o] = cloud;
		w[o + 1] = storm;
	}
	return values;
}

PackedFloat32Array WeatherNative::get_global_weather_rgba() const {
	return remap_sparse_severe_weather(get_global_weather_rgba_original(), global_atm);
}

PackedFloat32Array WeatherNative::get_local_weather_rgba() const {
	return remap_sparse_severe_weather(get_local_weather_rgba_original(), local_atm);
}

} // namespace godot
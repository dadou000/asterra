#include "weather_native.h"

#include <godot_cpp/core/class_db.hpp>
#include <algorithm>
#include <cmath>
#include <immintrin.h>
#include <numbers>

namespace godot {

static constexpr float PI_F = std::numbers::pi_v<float>;
static constexpr float HALF_PI_F = PI_F * 0.5f;
static constexpr float TAU_F = PI_F * 2.0f;
static constexpr float KAPPA = 0.286f;
// Asterra's locked 11.5-hour sidereal rotation. This drives Coriolis and
// planetary-vorticity terms in both the global and local atmosphere.
static constexpr float ROTATION_RATE = TAU_F / (11.5f * 3600.0f);
// Asterra's locked mean sea-level surface pressure.
static constexpr float P0 = 110000.0f;

// Coupled lower-boundary energy/water constants.
static constexpr float SIGMA_SB = 5.670374419e-8f;
static constexpr float CP_AIR = 1004.0f;
static constexpr float RD_AIR = 287.05f;
static constexpr float LV_WATER = 2.50e6f;
static constexpr float LF_ICE = 334000.0f;
static constexpr float CP_SNOW = 2100.0f;
static constexpr float BULK_HEAT_COEFF = 0.0014f;
static constexpr float BULK_MOISTURE_COEFF = 0.0013f;
static constexpr float MIN_EXCHANGE_WIND_MPS = 0.5f;
static constexpr float FREE_CONVECTION_WIND_COEFF = 0.65f;
static constexpr float BOTTOM_AIR_MASS_KG_M2 = 242.223f; // 0..~190 m pressure slab.
static constexpr float LAND_WATER_RESERVOIR_KG_M2 = 150.0f;
static constexpr float MAX_PRECIP_KG_M2_S = 0.008333333f; // 30 mm/h water equivalent.
static constexpr float LAND_DRY_CAPACITY_J_M2_K = 0.55e6f;
static constexpr float LAND_WET_CAPACITY_J_M2_K = 2.20e6f;
static constexpr float OCEAN_MIXED_CAPACITY_J_M2_K = 82.0e6f; // ~20 m mixed layer.
static constexpr float LAND_SUBSURFACE_CAPACITY_J_M2_K = 8.0e6f;
static constexpr float OCEAN_DEEP_CAPACITY_J_M2_K = 420.0e6f;
static constexpr float LAND_GROUND_CONDUCTANCE_W_M2_K = 7.0f;
static constexpr float OCEAN_DEEP_CONDUCTANCE_W_M2_K = 1.8f;
static constexpr float FRESH_SNOW_ALBEDO = 0.89f;
static constexpr float AGED_SNOW_ALBEDO = 0.72f;
static constexpr float WET_SNOW_ALBEDO = 0.60f;
static constexpr float OCEAN_ALBEDO = 0.060f;
static constexpr float SNOW_COVER_EFOLD_KG_M2 = 12.0f;
static constexpr float SNOW_ALBEDO_AGE_TAU_S = 5.0f * 11.5f * 3600.0f;
static constexpr std::array<int, 10> HORIZON_MARCH_CELLS = {
	1, 2, 3, 5, 8, 12, 18, 27, 40, 55
};

static constexpr std::array<float, WeatherNative::LAYERS> WIND_DRAG_TAU_S = {
	172800.000000f, 172800.000000f, 172800.000000f, 190080.000000f, 210816.000000f,
	235008.000000f, 263250.000000f, 299700.000000f, 340200.000000f, 384750.000000f,
	461113.000000f, 546573.900000f, 638608.700000f, 753765.500000f, 896772.400000f,
	1048717.200000f, 1209600.000000f, 1354269.800000f, 1498939.500000f, 1643609.300000f,
	1788279.100000f, 1900800.000000f, 1900800.000000f, 1900800.000000f, 1900800.000000f,
	1900800.000000f, 1900800.000000f, 1900800.000000f, 1900800.000000f, 1900800.000000f
};
static constexpr std::array<float, WeatherNative::LAYERS> THERMAL_RELAX_TAU_S = {
	172800.000000f, 172800.000000f, 172800.000000f, 181440.000000f, 191808.000000f,
	203904.000000f, 217350.000000f, 229500.000000f, 243000.000000f, 257850.000000f,
	279860.900000f, 304278.300000f, 330573.900000f, 366455.200000f, 414124.100000f,
	464772.400000f, 518400.000000f, 554567.400000f, 590734.900000f, 626902.300000f,
	663069.800000f, 691200.000000f, 691200.000000f, 691200.000000f, 691200.000000f,
	691200.000000f, 691200.000000f, 691200.000000f, 691200.000000f, 691200.000000f
};
static constexpr std::array<float, WeatherNative::LAYERS> MOISTURE_RELAX_TAU_S = {
	86400.000000f, 86400.000000f, 86400.000000f, 95040.000000f, 105408.000000f,
	117504.000000f, 132300.000000f, 156600.000000f, 183600.000000f, 213300.000000f,
	246991.300000f, 283617.400000f, 323060.900000f, 366455.200000f, 414124.100000f,
	464772.400000f, 518400.000000f, 554567.400000f, 590734.900000f, 626902.300000f,
	663069.800000f, 691200.000000f, 691200.000000f, 691200.000000f, 691200.000000f,
	691200.000000f, 691200.000000f, 691200.000000f, 691200.000000f, 691200.000000f
};

static inline float relaxation_fraction(float dt, float tau, float weight) {
	return 1.0f - std::exp(-std::max(weight, 0.0f) * dt / tau);
}

static inline float spherical_vector_sign(int y, int h) {
	float sign = 1.0f;
	while (y < 0 || y >= h) {
		if (y < 0) {
			y = -y - 1;
		} else {
			y = 2 * h - y - 1;
		}
		sign = -sign;
	}
	return sign;
}

static inline float smoothstep01(float x) {
	x = std::clamp(x, 0.0f, 1.0f);
	return x * x * (3.0f - 2.0f * x);
}

static inline __m256 clamp8(__m256 v, float lo, float hi) {
	return _mm256_min_ps(_mm256_set1_ps(hi), _mm256_max_ps(_mm256_set1_ps(lo), v));
}

static inline __m256 abs8(__m256 v) {
	const __m256 sign_mask = _mm256_set1_ps(-0.0f);
	return _mm256_andnot_ps(sign_mask, v);
}

// Cephes-style AVX exponential. This keeps saturation vapour pressure inside the
// vectorised hot loop instead of falling back to eight scalar std::exp calls.
static inline __m256 exp8(__m256 x) {
	const __m256 exp_hi = _mm256_set1_ps(88.3762626647949f);
	const __m256 exp_lo = _mm256_set1_ps(-88.3762626647949f);
	x = _mm256_min_ps(x, exp_hi);
	x = _mm256_max_ps(x, exp_lo);

	__m256 fx = _mm256_fmadd_ps(x, _mm256_set1_ps(1.44269504088896341f), _mm256_set1_ps(0.5f));
	__m256i emm0 = _mm256_cvttps_epi32(fx);
	__m256 tmp = _mm256_cvtepi32_ps(emm0);
	__m256 mask = _mm256_cmp_ps(tmp, fx, _CMP_GT_OS);
	mask = _mm256_and_ps(mask, _mm256_set1_ps(1.0f));
	fx = _mm256_sub_ps(tmp, mask);

	x = _mm256_fnmadd_ps(fx, _mm256_set1_ps(0.693359375f), x);
	x = _mm256_fnmadd_ps(fx, _mm256_set1_ps(-2.12194440e-4f), x);

	__m256 z = _mm256_mul_ps(x, x);
	__m256 y = _mm256_set1_ps(1.9875691500E-4f);
	y = _mm256_fmadd_ps(y, x, _mm256_set1_ps(1.3981999507E-3f));
	y = _mm256_fmadd_ps(y, x, _mm256_set1_ps(8.3334519073E-3f));
	y = _mm256_fmadd_ps(y, x, _mm256_set1_ps(4.1665795894E-2f));
	y = _mm256_fmadd_ps(y, x, _mm256_set1_ps(1.6666665459E-1f));
	y = _mm256_fmadd_ps(y, x, _mm256_set1_ps(5.0000001201E-1f));
	y = _mm256_fmadd_ps(y, z, x);
	y = _mm256_add_ps(y, _mm256_set1_ps(1.0f));

	emm0 = _mm256_cvttps_epi32(fx);
	emm0 = _mm256_add_epi32(emm0, _mm256_set1_epi32(0x7f));
	emm0 = _mm256_slli_epi32(emm0, 23);
	__m256 pow2n = _mm256_castsi256_ps(emm0);
	return _mm256_mul_ps(y, pow2n);
}

static inline __m256 qsat8(__m256 temperature_k, __m256 pressure_pa) {
	// Bolton/Tetens form, converted to specific humidity. Inputs are clamped to
	// the tropospheric range so extreme transient model states cannot overflow.
	__m256 t = clamp8(temperature_k, 205.0f, 318.0f);
	__m256 numerator = _mm256_mul_ps(_mm256_sub_ps(t, _mm256_set1_ps(273.15f)), _mm256_set1_ps(17.625f));
	__m256 denominator = _mm256_sub_ps(t, _mm256_set1_ps(30.11f));
	__m256 exponent = _mm256_div_ps(numerator, denominator);
	__m256 es = _mm256_mul_ps(_mm256_set1_ps(610.94f), exp8(exponent));
	__m256 denom_q = _mm256_sub_ps(pressure_pa, _mm256_mul_ps(es, _mm256_set1_ps(0.378f)));
	__m256 qsat = _mm256_div_ps(_mm256_mul_ps(es, _mm256_set1_ps(0.622f)), denom_q);
	return clamp8(qsat, 0.00002f, 0.040f);
}

static inline float sigma_temperature_factor(int layer) {
	return std::pow(WeatherNative::SIGMA[layer], KAPPA);
}

static inline int nearest_layer_for_height(float target_m) {
	int best = 0;
	float best_error = std::abs(WeatherNative::APPROX_HEIGHT_M[0] - target_m);
	for (int layer = 1; layer < WeatherNative::LAYERS; ++layer) {
		float error = std::abs(WeatherNative::APPROX_HEIGHT_M[layer] - target_m);
		if (error < best_error) {
			best_error = error;
			best = layer;
		}
	}
	return best;
}

static inline float old_six_level_column_scale() {
	return 6.0f / float(WeatherNative::LAYERS);
}

void WeatherNative::Atmosphere::resize(int p_width, int p_height) {
	width = p_width;
	height = p_height;
	cells = width * height;
	const size_t layered = size_t(cells) * LAYERS;
	const size_t interfaces = size_t(cells) * INTERFACES;
	for (auto *a : {&theta, &q, &u, &v, &liquid, &ice, &pressure,
		&ntheta, &nq, &nu, &nv, &nliquid, &nice, &npressure,
		&divergence, &vorticity, &potential_vorticity, &shear}) {
		a->assign(layered, 0.0f);
	}
	precip.assign(cells, 0.0f);
	nprecip.assign(cells, 0.0f);
	mass_flux.assign(interfaces, 0.0f);
}

void WeatherNative::SurfaceState::resize(int p_width, int p_height) {
	width = p_width;
	height = p_height;
	cells = width * height;
	for (auto *a : {&elevation_m, &water_fraction, &soil_moisture, &base_albedo,
		&dir_x, &dir_y, &dir_z, &normal_x, &normal_y, &normal_z,
		&temperature_k, &subsurface_temperature_k, &snow_swe_kg_m2,
		&snow_age_s, &snow_wetness, &albedo, &absorbed_solar_w_m2,
		&sensible_flux_w_m2, &latent_flux_w_m2, &ground_flux_w_m2}) {
		a->assign(cells, 0.0f);
	}
	horizon_tan.assign(size_t(cells) * HORIZON_SECTORS, 0.0f);
	sky_view_factor.assign(cells, 1.0f);
	terrain_sun_visibility.assign(cells, 1.0f);
}

int WeatherNative::wrap_x(int x, int w) {
	x %= w;
	return x < 0 ? x + w : x;
}

int WeatherNative::clamp_y(int y, int h) {
	return std::max(0, std::min(h - 1, y));
}

int WeatherNative::spherical_cell(int x, int y, int w, int h) {
	// A latitude/longitude grid crosses a pole into the same edge row at the
	// antipodal longitude. Reflecting the row and rotating half a circumference
	// supplies the correct cell-centred ghost topology for interpolation,
	// advection and centred north/south derivatives.
	while (y < 0 || y >= h) {
		if (y < 0) {
			y = -y - 1;
			x += w / 2;
		} else {
			y = 2 * h - y - 1;
			x += w / 2;
		}
	}
	return wrap_x(x, w) + y * w;
}

int WeatherNative::global_cell(int x, int y) {
	return wrap_x(x, GLOBAL_W) + clamp_y(y, GLOBAL_H) * GLOBAL_W;
}

int WeatherNative::local_cell(int x, int y) {
	return std::max(0, std::min(LOCAL_W - 1, x)) + std::max(0, std::min(LOCAL_H - 1, y)) * LOCAL_W;
}

WeatherNative::WeatherNative() {
	global_atm.resize(GLOBAL_W, GLOBAL_H);
	local_atm.resize(LOCAL_W, LOCAL_H);
	global_surface.resize(GLOBAL_W, GLOBAL_H);
	local_surface.resize(LOCAL_W, LOCAL_H);
	initialize_global_surface_geometry();
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

float WeatherNative::clamp01(float x) {
	return std::clamp(x, 0.0f, 1.0f);
}

int WeatherNative::tuning_index(const StringName &name) {
	if (name == StringName("circulation")) return CIRCULATION;
	if (name == StringName("temperature")) return TEMPERATURE;
	if (name == StringName("humidity")) return HUMIDITY;
	if (name == StringName("cloud_microphysics")) return CLOUD_MICROPHYSICS;
	if (name == StringName("convection")) return CONVECTION;
	if (name == StringName("precipitation")) return PRECIPITATION;
	return -1;
}

void WeatherNative::set_tuning_weight(const StringName &name, float value) {
	int index = tuning_index(name);
	if (index >= 0) tuning_weights[index] = std::clamp(value, 0.0f, 2.0f);
}

float WeatherNative::get_tuning_weight(const StringName &name) const {
	int index = tuning_index(name);
	return index >= 0 ? tuning_weights[index] : 0.0f;
}

void WeatherNative::reset_tuning_weights() {
	tuning_weights.fill(1.0f);
	layer_weights = DEFAULT_LAYER_WEIGHTS;
}

void WeatherNative::set_layer_weight(int layer, float value) {
	if (layer >= 0 && layer < LAYERS) layer_weights[layer] = std::clamp(value, 0.0f, 2.0f);
}

PackedFloat32Array WeatherNative::get_layer_weights() const {
	PackedFloat32Array out;
	out.resize(LAYERS);
	float *values = out.ptrw();
	for (int layer = 0; layer < LAYERS; ++layer) values[layer] = layer_weights[layer];
	return out;
}

float WeatherNative::actual_temperature(float theta, int layer) {
	return theta * sigma_temperature_factor(layer);
}

float WeatherNative::qsat_scalar(float temperature_k, float pressure_pa) {
	float t = std::clamp(temperature_k, 205.0f, 318.0f);
	float es = 610.94f * std::exp(17.625f * (t - 273.15f) / (t - 30.11f));
	return std::clamp(0.622f * es / std::max(pressure_pa - 0.378f * es, 1000.0f), 0.00002f, 0.040f);
}

void WeatherNative::initialize(int64_t p_seed) {
	seed = uint64_t(p_seed);
	global_simulation_seconds = 0.0;
	tropical_core_cell = {-1, -1};
	tropical_core_age_s = {0.0f, 0.0f};
	tropical_genesis_activity.assign(GLOBAL_W * GLOBAL_H, 0.0f);
	initialize_global();
	center_global_pressure(global_atm.pressure);
	diagnose(global_atm, true);
	local_initialized = false;
}

void WeatherNative::initialize_global() {
	const float phase = float(seed & 0xffffu) * 0.000173f;
	const float polar_cap_width = PI_F / 12.0f; // Smooth the final 15 degrees.
	for (int y = 0; y < GLOBAL_H; ++y) {
		float lat = HALF_PI_F - PI_F * (float(y) + 0.5f) / float(GLOBAL_H);
		float alat = std::abs(lat);
		float sinlat = std::sin(alat);
		// Longitude is undefined at a pole. Fade seeded zonal perturbations to a
		// single value there instead of squeezing full-strength waves into a ring
		// whose circumference tends to zero.
		float polar_taper = smoothstep01((HALF_PI_F - alat) / polar_cap_width);
		float surface_t = 302.0f - 48.0f * std::pow(sinlat, 1.35f);
		// Zonal relative humidity includes the observed dry subtropical belts.
		// Specific humidity is then diagnosed from saturation at each pressure
		// level instead of forcing unrealistically moist polar/upper air.
		float base_rh = std::clamp(0.78f
			- 0.24f * std::exp(-std::pow((alat - 0.43f) / 0.18f, 2.0f))
			+ 0.05f * std::pow(sinlat, 1.2f), 0.38f, 0.88f);

		for (int x = 0; x < GLOBAL_W; ++x) {
			int c = x + y * GLOBAL_W;
			float lon = TAU_F * (float(x) + 0.5f) / float(GLOBAL_W);
			float wave = polar_taper * (
				0.55f * std::sin(lon * 3.0f + lat * 1.7f + phase)
				+ 0.28f * std::sin(lon * 7.0f - lat * 2.3f + phase * 0.31f));
			float syn = polar_taper * (
				std::sin(lon * 2.0f + lat * 4.0f + phase * 3.1f)
				+ 0.35f * std::sin(lon * 5.0f - lat * 1.3f));

			for (int layer = 0; layer < LAYERS; ++layer) {
				int i = global_atm.layer_offset(layer) + c;
				float h = APPROX_HEIGHT_M[layer];
				float sf = sigma_temperature_factor(layer);
				float actual_t = std::clamp(surface_t - 0.00615f * h + wave * (2.1f - std::clamp(h / 12800.0f, 0.0f, 1.0f)), 205.0f, 310.0f);
				global_atm.theta[i] = actual_t / sf;

				float upper = std::clamp(h / 12800.0f, 0.0f, 1.0f);
				float jet = 7.0f + 34.0f * upper;
				jet *= std::exp(-std::pow((alat - 0.73f) / 0.27f, 2.0f));
				float trade = -9.0f * (1.0f - upper * 0.55f)
					* std::exp(-std::pow((alat - 0.30f) / 0.25f, 2.0f));
				global_atm.u[i] = jet + trade + wave * (3.0f + upper * 3.5f);
				global_atm.v[i] = (2.0f + upper * 2.0f) * std::cos(lon * 2.0f + lat * 3.0f + upper * 1.85f);
				global_atm.pressure[i] = syn * 850.0f * (0.95f - upper * 0.35f) + wave * 260.0f;

				float p_abs = P0 * SIGMA[layer] + global_atm.pressure[i];
				float sat = qsat_scalar(actual_t, p_abs);
				float rh = std::clamp(base_rh - 0.225f * upper + wave * 0.035f, 0.25f, 0.94f);
				global_atm.q[i] = std::clamp(sat * rh, 0.00001f, 0.024f);
				// Grid-box cloud begins below 100% mean RH to represent unresolved
				// saturated sub-columns; resolved condensation still uses true qsat.
				float supersat = std::max(global_atm.q[i] - sat * 0.96f, 0.0f);
				float ice_frac = clamp01((268.0f - actual_t) / 20.0f);
				global_atm.liquid[i] = supersat * (1.0f - ice_frac) * 0.20f;
				global_atm.ice[i] = supersat * ice_frac * 0.20f;
			}
			global_atm.precip[c] = 0.0f;
		}
	}
}

float WeatherNative::sample_global_layer(const std::vector<float> &field, int layer, float x, float y) const {
	int x0 = int(std::floor(x));
	int y0 = int(std::floor(y));
	float fx = x - float(x0);
	float fy = y - float(y0);
	int off = global_atm.layer_offset(layer);
	float a00 = field[off + spherical_cell(x0, y0, GLOBAL_W, GLOBAL_H)];
	float a10 = field[off + spherical_cell(x0 + 1, y0, GLOBAL_W, GLOBAL_H)];
	float a01 = field[off + spherical_cell(x0, y0 + 1, GLOBAL_W, GLOBAL_H)];
	float a11 = field[off + spherical_cell(x0 + 1, y0 + 1, GLOBAL_W, GLOBAL_H)];
	return std::lerp(std::lerp(a00, a10, fx), std::lerp(a01, a11, fx), fy);
}

float WeatherNative::sample_global_scalar(const std::vector<float> &field, float x, float y) const {
	int x0 = int(std::floor(x));
	int y0 = int(std::floor(y));
	float fx = x - float(x0);
	float fy = y - float(y0);
	float a00 = field[spherical_cell(x0, y0, GLOBAL_W, GLOBAL_H)];
	float a10 = field[spherical_cell(x0 + 1, y0, GLOBAL_W, GLOBAL_H)];
	float a01 = field[spherical_cell(x0, y0 + 1, GLOBAL_W, GLOBAL_H)];
	float a11 = field[spherical_cell(x0 + 1, y0 + 1, GLOBAL_W, GLOBAL_H)];
	return std::lerp(std::lerp(a00, a10, fx), std::lerp(a01, a11, fx), fy);
}

float WeatherNative::sample_global_surface_scalar(const std::vector<float> &field, const Vector3 &d) const {
	Vector3 n = d.normalized();
	float lon = std::atan2(n.z, n.x);
	if (lon < 0.0f) lon += TAU_F;
	float lat = std::asin(std::clamp(n.y, -1.0f, 1.0f));
	float x = lon / TAU_F * float(GLOBAL_W) - 0.5f;
	float y = (HALF_PI_F - lat) / PI_F * float(GLOBAL_H) - 0.5f;
	return sample_global_scalar(field, x, y);
}

void WeatherNative::initialize_global_surface_geometry() {
	for (int y = 0; y < GLOBAL_H; ++y) {
		float lat = HALF_PI_F - PI_F * (float(y) + 0.5f) / float(GLOBAL_H);
		float sin_lat = std::sin(lat);
		float cos_lat = std::cos(lat);
		for (int x = 0; x < GLOBAL_W; ++x) {
			float lon = TAU_F * (float(x) + 0.5f) / float(GLOBAL_W);
			int c = x + y * GLOBAL_W;
			Vector3 d(cos_lat * std::cos(lon), sin_lat, cos_lat * std::sin(lon));
			global_surface.dir_x[c] = d.x;
			global_surface.dir_y[c] = d.y;
			global_surface.dir_z[c] = d.z;
			global_surface.normal_x[c] = d.x;
			global_surface.normal_y[c] = d.y;
			global_surface.normal_z[c] = d.z;
		}
	}
}

void WeatherNative::update_global_surface_normals() {
	for (int y = 0; y < GLOBAL_H; ++y) {
		float lat = HALF_PI_F - PI_F * (float(y) + 0.5f) / float(GLOBAL_H);
		float cos_lat = std::cos(lat);
		float dx = std::max(TAU_F * PLANET_RADIUS_M * std::max(cos_lat, 0.01f) / float(GLOBAL_W), 1000.0f);
		float dy = PI_F * PLANET_RADIUS_M / float(GLOBAL_H);
		for (int x = 0; x < GLOBAL_W; ++x) {
			int c = x + y * GLOBAL_W;
			Vector3 d(global_surface.dir_x[c], global_surface.dir_y[c], global_surface.dir_z[c]);
			float lon = TAU_F * (float(x) + 0.5f) / float(GLOBAL_W);
			Vector3 east(-std::sin(lon), 0.0f, std::cos(lon));
			Vector3 north = east.cross(d).normalized();
			float h_e = global_surface.elevation_m[spherical_cell(x + 1, y, GLOBAL_W, GLOBAL_H)];
			float h_w = global_surface.elevation_m[spherical_cell(x - 1, y, GLOBAL_W, GLOBAL_H)];
			float h_n = global_surface.elevation_m[spherical_cell(x, y - 1, GLOBAL_W, GLOBAL_H)];
			float h_s = global_surface.elevation_m[spherical_cell(x, y + 1, GLOBAL_W, GLOBAL_H)];
			float slope_e = (h_e - h_w) * (0.5f / dx);
			float slope_n = (h_n - h_s) * (0.5f / dy);
			Vector3 terrain_normal = (d - east * slope_e - north * slope_n).normalized();
			// Free water follows the geoid rather than the underlying seabed/lake floor.
			float water = std::clamp(global_surface.water_fraction[c], 0.0f, 1.0f);
			Vector3 n = terrain_normal.lerp(d, water).normalized();
			global_surface.normal_x[c] = n.x;
			global_surface.normal_y[c] = n.y;
			global_surface.normal_z[c] = n.z;
		}
	}
}

void WeatherNative::initialize_global_surface_state() {
	const int l0 = global_atm.layer_offset(0);
	const float sf0 = sigma_temperature_factor(0);
	for (int c = 0; c < global_surface.cells; ++c) {
		float air_t = global_atm.theta[l0 + c] * sf0;
		float water = std::clamp(global_surface.water_fraction[c], 0.0f, 1.0f);
		float moisture = std::clamp(global_surface.soil_moisture[c], 0.0f, 1.0f);
		float terrain_lapse = std::max(global_surface.elevation_m[c], 0.0f) * 0.0045f;
		float land_t = air_t + 2.4f - terrain_lapse;
		float ocean_t = air_t + 1.0f;
		float ts = std::clamp(std::lerp(land_t, ocean_t, water), 225.0f, 315.0f);
		global_surface.temperature_k[c] = ts;
		global_surface.subsurface_temperature_k[c] = ts;
		float cold = std::clamp((273.15f - ts) / 12.0f, 0.0f, 1.0f);
		global_surface.snow_swe_kg_m2[c] = (1.0f - water) * cold * 60.0f;
		global_surface.snow_age_s[c] = cold > 0.01f ? 2.0f * 11.5f * 3600.0f : 0.0f;
		global_surface.snow_wetness[c] = 0.0f;
		global_surface.albedo[c] = std::lerp(
			std::clamp(global_surface.base_albedo[c], 0.05f, 0.60f), OCEAN_ALBEDO, water);
		global_surface.absorbed_solar_w_m2[c] = 0.0f;
		global_surface.sensible_flux_w_m2[c] = 0.0f;
		global_surface.latent_flux_w_m2[c] = 0.0f;
		global_surface.ground_flux_w_m2[c] = 0.0f;
		global_surface.soil_moisture[c] = moisture;
	}
}

void WeatherNative::set_global_surface_fields(const PackedFloat32Array &fields) {
	if (fields.size() != GLOBAL_W * GLOBAL_H * 4) {
		return;
	}
	for (int c = 0; c < global_surface.cells; ++c) {
		global_surface.elevation_m[c] = fields[c * 4 + 0];
		global_surface.water_fraction[c] = std::clamp(fields[c * 4 + 1], 0.0f, 1.0f);
		global_surface.soil_moisture[c] = std::clamp(fields[c * 4 + 2], 0.0f, 1.0f);
		global_surface.base_albedo[c] = std::clamp(fields[c * 4 + 3], 0.04f, 0.65f);
	}
	update_global_surface_normals();
	initialize_global_surface_state();
	surface_fields_ready = true;
	local_surface_fields_ready = false;
	// Rebuild the nest so its lower boundary is sampled from the new geography
	// rather than retaining the all-zero placeholder surface.
	local_initialized = false;
}

void WeatherNative::set_solar_forcing(const Vector3 &sun_direction_body, float irradiance_w_m2,
		float angular_radius_rad) {
	if (sun_direction_body.length_squared() > 1e-10f) {
		solar_direction_body = sun_direction_body.normalized();
	}
	solar_irradiance_w_m2 = std::clamp(irradiance_w_m2, 0.0f, 5000.0f);
	helion_angular_radius_rad = std::clamp(angular_radius_rad, 0.0001f, 0.03f);
}

void WeatherNative::swap_state(Atmosphere &a) {
	a.theta.swap(a.ntheta);
	a.q.swap(a.nq);
	a.u.swap(a.nu);
	a.v.swap(a.nv);
	a.liquid.swap(a.nliquid);
	a.ice.swap(a.nice);
	a.pressure.swap(a.npressure);
	a.precip.swap(a.nprecip);
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

void WeatherNative::filter_global_poles(Atmosphere &a) {
	// Longitude rings contain progressively less independent information as their
	// circumference shrinks. Retain the true metric, but average each polar ring
	// over a continuously widening longitude footprint. This is the finite-volume
	// equivalent of a reduced grid: at least two equatorial cell widths contribute
	// everywhere, without introducing a hard cap edge or erasing planetary waves.
	std::vector<float> row_buffer(a.width);
	auto filter_row = [&](std::vector<float> &field, int offset, int y,
			int radius, float fractional_blend) {
		const int row = offset + y * a.width;
		const int samples = radius * 2 + 1;
		double sum = 0.0;
		for (int k = -radius; k <= radius; ++k) {
			sum += field[row + wrap_x(k, a.width)];
		}
		for (int x = 0; x < a.width; ++x) {
			row_buffer[x] = std::lerp(field[row + x], float(sum / samples), fractional_blend);
			sum -= field[row + wrap_x(x - radius, a.width)];
			sum += field[row + wrap_x(x + radius + 1, a.width)];
		}
		std::copy(row_buffer.begin(), row_buffer.end(), field.begin() + row);
	};

	for (int y = 0; y < a.height; ++y) {
		float lat = HALF_PI_F - PI_F * (float(y) + 0.5f) / float(a.height);
		float coslat = std::max(std::cos(lat), 1.0e-4f);
		float desired_radius = std::max(1.0f / coslat - 0.5f, 0.0f);
		if (desired_radius < 1.0f) continue;
		int radius = std::min(int(std::ceil(desired_radius)), a.width / 2);
		float blend = std::clamp(desired_radius - float(radius - 1), 0.0f, 1.0f);
		for (int layer = 0; layer < LAYERS; ++layer) {
			int off = a.layer_offset(layer);
			filter_row(a.ntheta, off, y, radius, blend);
			filter_row(a.nq, off, y, radius, blend);
			filter_row(a.nu, off, y, radius, blend);
			filter_row(a.nv, off, y, radius, blend);
			filter_row(a.nliquid, off, y, radius, blend);
			filter_row(a.nice, off, y, radius, blend);
			filter_row(a.npressure, off, y, radius, blend);
		}
		filter_row(a.nprecip, 0, y, radius, blend);
		for (int interface_index = 0; interface_index < INTERFACES; ++interface_index) {
			filter_row(a.mass_flux, a.interface_offset(interface_index), y, radius, blend);
		}
		// A northward vector cannot cross the coordinate singularity unchanged.
		// Only the innermost cell is tapered; the continuous filter handles the rest.
		float pole_taper = std::clamp(coslat * float(a.height), 0.0f, 1.0f);
		if (pole_taper < 1.0f) {
			for (int layer = 0; layer < LAYERS; ++layer) {
				int row = a.layer_offset(layer) + y * a.width;
				for (int x = 0; x < a.width; ++x) a.nv[row + x] *= pole_taper;
			}
		}
	}

	// The meridional spacing stays finite at the pole, but the two innermost
	// rings share an ever larger fraction of the same physical control volume.
	// Diffuse them together with a strength that rises continuously from zero at
	// 60 degrees. Vector ghosts receive the basis sign used by the dynamics.
	std::vector<float> meridional_source;
	auto meridional_filter = [&](std::vector<float> &field, float peak_strength,
			bool vector_component) {
		meridional_source.assign(field.begin(), field.end());
		for (int y = 0; y < a.height; ++y) {
			float lat = HALF_PI_F - PI_F * (float(y) + 0.5f) / float(a.height);
			float coslat = std::cos(lat);
			if (coslat >= 0.5f) continue;
			float strength = peak_strength * (1.0f - smoothstep01(coslat / 0.5f));
			float north_basis = vector_component ? spherical_vector_sign(y - 1, a.height) : 1.0f;
			float south_basis = vector_component ? spherical_vector_sign(y + 1, a.height) : 1.0f;
			for (int layer = 0; layer < int(field.size()) / (a.width * a.height); ++layer) {
				int off = layer * a.width * a.height;
				for (int x = 0; x < a.width; ++x) {
					int c = off + x + y * a.width;
					float north = meridional_source[off + spherical_cell(x, y - 1, a.width, a.height)] * north_basis;
					float south = meridional_source[off + spherical_cell(x, y + 1, a.width, a.height)] * south_basis;
					float average = 0.25f * north + 0.5f * meridional_source[c] + 0.25f * south;
					field[c] = std::lerp(meridional_source[c], average, strength);
				}
			}
		}
	};
	meridional_filter(a.npressure, 0.58f, false);
	meridional_filter(a.nu, 0.44f, true);
	meridional_filter(a.nv, 0.44f, true);
	meridional_filter(a.ntheta, 0.18f, false);
	meridional_filter(a.nq, 0.18f, false);
	meridional_filter(a.nliquid, 0.18f, false);
	meridional_filter(a.nice, 0.18f, false);
	meridional_filter(a.nprecip, 0.18f, false);
	meridional_filter(a.mass_flux, 0.36f, false);
}

void WeatherNative::center_global_pressure(std::vector<float> &pressure) {
	// Pressure is stored as a perturbation. Removing each level's area-weighted
	// global mean prevents moist/radiative closures from creating or destroying
	// atmospheric mass. Latitude rows represent different physical areas.
	#pragma omp parallel for schedule(static)
	for (int layer = 0; layer < LAYERS; ++layer) {
		double weighted_sum = 0.0;
		double area_sum = 0.0;
		int off = global_atm.layer_offset(layer);
		for (int y = 0; y < GLOBAL_H; ++y) {
			float lat = HALF_PI_F - PI_F * (float(y) + 0.5f) / float(GLOBAL_H);
			double area_weight = std::max(double(std::cos(lat)), 0.0);
			for (int x = 0; x < GLOBAL_W; ++x) {
				weighted_sum += double(pressure[off + x + y * GLOBAL_W]) * area_weight;
				area_sum += area_weight;
			}
		}
		float mean = area_sum > 0.0 ? float(weighted_sum / area_sum) : 0.0f;
		for (int c = 0; c < global_atm.cells; ++c) {
			pressure[off + c] = std::clamp(pressure[off + c] - mean, -6500.0f, 6500.0f);
		}
	}
}

void WeatherNative::horizontal_pass(Atmosphere &a, bool is_global, float dt) {
	const __m256 zero = _mm256_setzero_ps();
	const int w = a.width;
	const int h = a.height;
	const float local_lat = std::asin(std::clamp(local_center.y, -1.0f, 1.0f));
	const float circulation_weight = tuning_weights[CIRCULATION];
	const float temperature_weight = tuning_weights[TEMPERATURE];
	const float humidity_weight = tuning_weights[HUMIDITY];
	const float cloud_weight = tuning_weights[CLOUD_MICROPHYSICS];
	const float precipitation_weight = tuning_weights[PRECIPITATION];

	// Precipitation is a column field. It decays between fallout pulses so the
	// output represents current intensity rather than accumulated rainfall.
	float precip_memory = std::exp(-dt / 900.0f);
	for (int i = 0; i < a.cells; i += 8) {
		__m256 p = _mm256_loadu_ps(&a.precip[i]);
		_mm256_storeu_ps(&a.nprecip[i], _mm256_mul_ps(p, _mm256_set1_ps(precip_memory)));
	}

	for (int layer = 0; layer < LAYERS; ++layer) {
		const int off = a.layer_offset(layer);
		const float sigma = SIGMA[layer];
		const float sf = sigma_temperature_factor(layer);
		const float height_m = APPROX_HEIGHT_M[layer];
		const float rho = 1.33f * std::pow(sigma, 0.82f);
		const float wind_damp = std::exp(-dt / WIND_DRAG_TAU_S[layer]);
		const float wind_relax = relaxation_fraction(dt, 259200.0f, circulation_weight);
		const float theta_relax = relaxation_fraction(dt, THERMAL_RELAX_TAU_S[layer], temperature_weight);
		const float q_relax = relaxation_fraction(dt, MOISTURE_RELAX_TAU_S[layer], humidity_weight);
		const float cond_frac = std::min(cloud_weight * dt / 120.0f, 1.0f);
		const float cloud_decay = std::exp(-cloud_weight * dt / 57600.0f);
		const float pressure_relax = relaxation_fraction(dt, 129600.0f, circulation_weight);
		// A one-hour global Laplacian erased every mesoscale pressure core before
		// Coriolis could close its circulation. The 90 s global integrator is stable
		// with a three-hour scale; the kilometre nest retains the tighter filter.
		const float pressure_smooth = relaxation_fraction(
			dt, is_global ? 10800.0f : 3600.0f, 1.0f);

		#pragma omp parallel for schedule(static)
		for (int y = 0; y < h; ++y) {
			alignas(32) int adv00_idx[8], adv10_idx[8], adv01_idx[8], adv11_idx[8];
			alignas(32) int east_idx[8], west_idx[8], north_idx[8], south_idx[8];
			alignas(32) float adv_fx[8], adv_fy[8], target_u_lane[8], target_v_lane[8];
			alignas(32) float adv00_sign[8], adv10_sign[8], adv01_sign[8], adv11_sign[8];
			alignas(32) float north_sign[8], south_sign[8];
			float lat = is_global
				? HALF_PI_F - PI_F * (float(y) + 0.5f) / float(h)
				: local_lat;
			float coslat = std::cos(lat);
			float dx = is_global ? TAU_F * PLANET_RADIUS_M * coslat / float(w) : LOCAL_CELL_M;
			float dy = is_global ? PI_F * PLANET_RADIUS_M / float(h) : LOCAL_CELL_M;
			float coriolis = 2.0f * ROTATION_RATE * std::sin(lat);
			float curvature = is_global ? std::tan(lat) / PLANET_RADIUS_M : 0.0f;
			float alat = std::abs(lat);
			float sinlat = std::sin(alat);
			float surface_t = 302.0f - 48.0f * std::pow(sinlat, 1.35f);
			float clim_actual_t = std::clamp(surface_t - 0.00615f * height_m, 205.0f, 310.0f);
			float clim_theta = clim_actual_t / sf;
			float clim_rh = std::clamp(0.78f
				- 0.24f * std::exp(-std::pow((alat - 0.43f) / 0.18f, 2.0f))
				+ 0.05f * std::pow(sinlat, 1.2f) - 0.225f * std::clamp(height_m / 12800.0f, 0.0f, 1.0f), 0.25f, 0.88f);
			float clim_q = std::clamp(qsat_scalar(clim_actual_t, P0 * sigma) * clim_rh, 0.00001f, 0.024f);
			float upper = std::clamp(height_m / 12800.0f, 0.0f, 1.0f);
			float target_u = (7.0f + 34.0f * upper)
				* std::exp(-std::pow((alat - 0.73f) / 0.27f, 2.0f));
			target_u += -9.0f * (1.0f - upper * 0.55f)
				* std::exp(-std::pow((alat - 0.30f) / 0.25f, 2.0f));
			float wave_envelope = std::exp(-std::pow((alat - 0.66f) / 0.36f, 2.0f));
			float seed_phase = float(seed & 0xffffu) * 0.000173f;
			float wave_translation = float(global_simulation_seconds / 86400.0) * 0.349066f;

			for (int x = 0; x < w; x += 8) {
				for (int k = 0; k < 8; ++k) {
					int xx = x + k;
					if (is_global) {
						float lon = TAU_F * (float(xx) + 0.5f) / float(w);
						float phase3 = 3.0f * (lon - wave_translation) + 1.7f * lat + seed_phase;
						float phase5 = 5.0f * (lon - wave_translation * 1.15f) - 1.2f * lat + seed_phase * 0.37f;
						float phase7 = 7.0f * (lon - wave_translation * 0.82f) + 0.8f * lat - seed_phase * 0.21f;
						float amplitude = wave_envelope * (5.0f + 7.0f * upper);
						target_u_lane[k] = target_u + amplitude * std::cos(phase3)
							+ amplitude * (0.32f * std::sin(phase5) + 0.16f * std::cos(phase7));
						target_v_lane[k] = amplitude * 0.78f * std::sin(phase3)
							+ amplitude * (0.26f * std::cos(phase5) + 0.14f * std::sin(phase7));
					} else {
						// The nest axes are transported tangent-plane axes, not geographic
						// east/north. Its large-scale circulation enters through the rim.
						target_u_lane[k] = 0.0f;
						target_v_lane[k] = 0.0f;
					}
					int c = is_global ? global_cell(xx, y) : local_cell(xx, y);
					float cu = a.u[off + c];
					float cv = a.v[off + c];
					float source_x = float(xx) - cu * dt / dx;
					float source_y = float(y) + (is_global ? cv : -cv) * dt / dy;
					if (!is_global) {
						source_x = std::clamp(source_x, 0.0f, float(w - 1));
						source_y = std::clamp(source_y, 0.0f, float(h - 1));
					}
					int x0 = int(std::floor(source_x));
					int y0 = int(std::floor(source_y));
					adv_fx[k] = source_x - float(x0);
					adv_fy[k] = source_y - float(y0);
					if (is_global) {
						adv00_idx[k] = spherical_cell(x0, y0, w, h);
						adv10_idx[k] = spherical_cell(x0 + 1, y0, w, h);
						adv01_idx[k] = spherical_cell(x0, y0 + 1, w, h);
						adv11_idx[k] = spherical_cell(x0 + 1, y0 + 1, w, h);
						adv00_sign[k] = adv10_sign[k] = spherical_vector_sign(y0, h);
						adv01_sign[k] = adv11_sign[k] = spherical_vector_sign(y0 + 1, h);
					} else {
						adv00_idx[k] = local_cell(x0, y0);
						adv10_idx[k] = local_cell(x0 + 1, y0);
						adv01_idx[k] = local_cell(x0, y0 + 1);
						adv11_idx[k] = local_cell(x0 + 1, y0 + 1);
						adv00_sign[k] = adv10_sign[k] = 1.0f;
						adv01_sign[k] = adv11_sign[k] = 1.0f;
					}
					east_idx[k] = (is_global ? wrap_x(xx + 1, w) : std::min(xx + 1, w - 1)) + y * w;
					west_idx[k] = (is_global ? wrap_x(xx - 1, w) : std::max(xx - 1, 0)) + y * w;
					north_idx[k] = is_global
						? spherical_cell(xx, y - 1, w, h)
						: xx + std::min(y + 1, h - 1) * w;
					south_idx[k] = is_global
						? spherical_cell(xx, y + 1, w, h)
						: xx + std::max(y - 1, 0) * w;
					north_sign[k] = is_global ? spherical_vector_sign(y - 1, h) : 1.0f;
					south_sign[k] = is_global ? spherical_vector_sign(y + 1, h) : 1.0f;
				}

				__m256i a00i = _mm256_load_si256(reinterpret_cast<const __m256i *>(adv00_idx));
				__m256i a10i = _mm256_load_si256(reinterpret_cast<const __m256i *>(adv10_idx));
				__m256i a01i = _mm256_load_si256(reinterpret_cast<const __m256i *>(adv01_idx));
				__m256i a11i = _mm256_load_si256(reinterpret_cast<const __m256i *>(adv11_idx));
				__m256 fxv = _mm256_load_ps(adv_fx);
				__m256 fyv = _mm256_load_ps(adv_fy);
				__m256 a00sv = _mm256_load_ps(adv00_sign);
				__m256 a10sv = _mm256_load_ps(adv10_sign);
				__m256 a01sv = _mm256_load_ps(adv01_sign);
				__m256 a11sv = _mm256_load_ps(adv11_sign);
				__m256 north_sv = _mm256_load_ps(north_sign);
				__m256 south_sv = _mm256_load_ps(south_sign);
				__m256 target_uv = _mm256_load_ps(target_u_lane);
				__m256 target_vv = _mm256_load_ps(target_v_lane);
				__m256i ei = _mm256_load_si256(reinterpret_cast<const __m256i *>(east_idx));
				__m256i wi = _mm256_load_si256(reinterpret_cast<const __m256i *>(west_idx));
				__m256i ni = _mm256_load_si256(reinterpret_cast<const __m256i *>(north_idx));
				__m256i si = _mm256_load_si256(reinterpret_cast<const __m256i *>(south_idx));

				const float *theta_base = a.theta.data() + off;
				const float *q_base = a.q.data() + off;
				const float *u_base = a.u.data() + off;
				const float *v_base = a.v.data() + off;
				const float *liq_base = a.liquid.data() + off;
				const float *ice_base = a.ice.data() + off;
				const float *p_base = a.pressure.data() + off;
				int base = x + y * w;

				auto advect = [&](const float *field, bool vector_component = false) {
					__m256 a00 = _mm256_i32gather_ps(field, a00i, 4);
					__m256 a10 = _mm256_i32gather_ps(field, a10i, 4);
					__m256 a01 = _mm256_i32gather_ps(field, a01i, 4);
					__m256 a11 = _mm256_i32gather_ps(field, a11i, 4);
					if (vector_component) {
						a00 = _mm256_mul_ps(a00, a00sv);
						a10 = _mm256_mul_ps(a10, a10sv);
						a01 = _mm256_mul_ps(a01, a01sv);
						a11 = _mm256_mul_ps(a11, a11sv);
					}
					__m256 row0 = _mm256_fmadd_ps(fxv, _mm256_sub_ps(a10, a00), a00);
					__m256 row1 = _mm256_fmadd_ps(fxv, _mm256_sub_ps(a11, a01), a01);
					return _mm256_fmadd_ps(fyv, _mm256_sub_ps(row1, row0), row0);
				};
				__m256 theta = advect(theta_base);
				__m256 q = advect(q_base);
				__m256 u = advect(u_base, true);
				__m256 v = advect(v_base, true);
				__m256 liquid = advect(liq_base);
				__m256 ice = advect(ice_base);
				__m256 pressure = advect(p_base);

				__m256 pe = _mm256_i32gather_ps(p_base, ei, 4);
				__m256 pw = _mm256_i32gather_ps(p_base, wi, 4);
				__m256 pn = _mm256_i32gather_ps(p_base, ni, 4);
				__m256 ps = _mm256_i32gather_ps(p_base, si, 4);
				__m256 ue = _mm256_i32gather_ps(u_base, ei, 4);
				__m256 uw = _mm256_i32gather_ps(u_base, wi, 4);
				__m256 un = _mm256_i32gather_ps(u_base, ni, 4);
				__m256 us = _mm256_i32gather_ps(u_base, si, 4);
				__m256 ve = _mm256_i32gather_ps(v_base, ei, 4);
				__m256 vw = _mm256_i32gather_ps(v_base, wi, 4);
				__m256 vn = _mm256_i32gather_ps(v_base, ni, 4);
				__m256 vs = _mm256_i32gather_ps(v_base, si, 4);
				un = _mm256_mul_ps(un, north_sv);
				us = _mm256_mul_ps(us, south_sv);
				vn = _mm256_mul_ps(vn, north_sv);
				vs = _mm256_mul_ps(vs, south_sv);
				__m256 center_u = _mm256_loadu_ps(&a.u[off + base]);
				__m256 center_v = _mm256_loadu_ps(&a.v[off + base]);

				__m256 dpde = _mm256_mul_ps(_mm256_sub_ps(pe, pw), _mm256_set1_ps(0.5f / dx));
				__m256 dpdn = _mm256_mul_ps(_mm256_sub_ps(pn, ps), _mm256_set1_ps(0.5f / dy));
				__m256 div = _mm256_add_ps(
					_mm256_mul_ps(_mm256_sub_ps(ue, uw), _mm256_set1_ps(0.5f / dx)),
					_mm256_mul_ps(_mm256_sub_ps(vn, vs), _mm256_set1_ps(0.5f / dy)));
				div = _mm256_fnmadd_ps(center_v, _mm256_set1_ps(curvature), div);
				__m256 vort = _mm256_sub_ps(
					_mm256_mul_ps(_mm256_sub_ps(ve, vw), _mm256_set1_ps(0.5f / dx)),
					_mm256_mul_ps(_mm256_sub_ps(un, us), _mm256_set1_ps(0.5f / dy)));
				vort = _mm256_fmadd_ps(center_u, _mm256_set1_ps(curvature), vort);

				_mm256_storeu_ps(&a.divergence[off + x + y * w], div);
				_mm256_storeu_ps(&a.vorticity[off + x + y * w], vort);

				// Horizontal momentum in local east/north coordinates.
				__m256 old_u = u;
				__m256 effective_coriolis = _mm256_fmadd_ps(
					old_u, _mm256_set1_ps(curvature), _mm256_set1_ps(coriolis));
				u = _mm256_fmadd_ps(
					_mm256_sub_ps(_mm256_mul_ps(v, effective_coriolis), _mm256_div_ps(dpde, _mm256_set1_ps(rho))),
					_mm256_set1_ps(dt), u);
				v = _mm256_fmadd_ps(
					_mm256_sub_ps(_mm256_sub_ps(zero, _mm256_div_ps(dpdn, _mm256_set1_ps(rho))), _mm256_mul_ps(old_u, effective_coriolis)),
					_mm256_set1_ps(dt), v);
				u = _mm256_mul_ps(u, _mm256_set1_ps(wind_damp));
				v = _mm256_mul_ps(v, _mm256_set1_ps(wind_damp));
				if (is_global) {
					u = _mm256_fmadd_ps(_mm256_sub_ps(target_uv, u), _mm256_set1_ps(wind_relax), u);
					v = _mm256_fmadd_ps(_mm256_sub_ps(target_vv, v), _mm256_set1_ps(wind_relax), v);
				}

				// Thermodynamic relaxation supplies large-scale radiative/surface forcing.
				theta = _mm256_fmadd_ps(_mm256_sub_ps(_mm256_set1_ps(clim_theta), theta), _mm256_set1_ps(theta_relax), theta);
				q = _mm256_fmadd_ps(_mm256_sub_ps(_mm256_set1_ps(clim_q), q), _mm256_set1_ps(q_relax), q);

				// Pressure/geopotential anomalies react to thermal anomalies and horizontal
				// convergence. A small Laplacian damps grid-scale acoustic noise.
				__m256 thermal = _mm256_sub_ps(theta, _mm256_set1_ps(clim_theta));
				__m256 moisture = _mm256_sub_ps(q, _mm256_set1_ps(clim_q));
				__m256 target_p = _mm256_sub_ps(
					_mm256_mul_ps(thermal, _mm256_set1_ps(-900.0f * sigma)),
					_mm256_mul_ps(moisture, _mm256_set1_ps(120000.0f)));
				pressure = _mm256_fmadd_ps(_mm256_sub_ps(target_p, pressure), _mm256_set1_ps(pressure_relax), pressure);
				// The global mesh is strongly damped and can use the compact explicit
				// pressure/divergence update. The kilometre-scale nest applies this
				// tendency from the newly accelerated wind below, forming a stable
				// symplectic pressure-wave step instead of a growing forward-Euler pair.
				if (is_global) {
					pressure = _mm256_fnmadd_ps(div, _mm256_set1_ps(8000.0f * sigma * dt * circulation_weight), pressure);
				}
				__m256 pavg = _mm256_mul_ps(_mm256_add_ps(_mm256_add_ps(pe, pw), _mm256_add_ps(pn, ps)), _mm256_set1_ps(0.25f));
				pressure = _mm256_fmadd_ps(_mm256_sub_ps(pavg, pressure), _mm256_set1_ps(pressure_smooth), pressure);

				// Moist microphysics. q is true specific humidity; cloud liquid/ice are
				// condensate mixing ratios. Latent heating feeds back into theta.
				__m256 temperature = _mm256_mul_ps(theta, _mm256_set1_ps(sf));
				__m256 pabs = clamp8(_mm256_add_ps(_mm256_set1_ps(P0 * sigma), pressure), 12000.0f, 115000.0f);
				__m256 qsat = qsat8(temperature, pabs);
				__m256 supersat = _mm256_max_ps(_mm256_sub_ps(q, qsat), zero);
				__m256 resolved_cond = _mm256_mul_ps(supersat, _mm256_set1_ps(cond_frac));
				// A grid box becomes partly cloudy before its mean humidity reaches
				// saturation. Slowly condense the sub-grid saturated fraction while
				// retaining the fast resolved-supersaturation adjustment above.
				__m256 subgrid_excess = _mm256_max_ps(
					_mm256_sub_ps(q, _mm256_mul_ps(qsat, _mm256_set1_ps(0.68f))), zero);
				__m256 subgrid_cond = _mm256_mul_ps(
					subgrid_excess, _mm256_set1_ps(std::min(cloud_weight * dt / 43200.0f, 0.02f)));
				// Resolved convergence concentrates vapour along fronts. Coupling a
				// small part of that compression directly to condensate preserves the
				// narrow comma bands that a 30-level grid would otherwise smear out.
				__m256 convergent_fraction = clamp8(_mm256_mul_ps(
					_mm256_max_ps(_mm256_sub_ps(zero, div), zero),
					_mm256_set1_ps(dt * 0.16f * cloud_weight)), 0.0f, 0.015f);
				__m256 convergence_cond = _mm256_mul_ps(q, convergent_fraction);
				__m256 cond = _mm256_min_ps(q,
					_mm256_add_ps(_mm256_max_ps(resolved_cond, subgrid_cond), convergence_cond));
				q = _mm256_sub_ps(q, cond);
				__m256 ice_frac = clamp8(_mm256_mul_ps(_mm256_sub_ps(_mm256_set1_ps(268.0f), temperature), _mm256_set1_ps(1.0f / 20.0f)), 0.0f, 1.0f);
				liquid = _mm256_fmadd_ps(cond, _mm256_sub_ps(_mm256_set1_ps(1.0f), ice_frac), liquid);
				ice = _mm256_fmadd_ps(cond, ice_frac, ice);
				theta = _mm256_fmadd_ps(cond, _mm256_set1_ps(2500.0f / sf), theta);

				// Sub-saturated air re-evaporates cloud water, cooling the layer.
				__m256 deficit = _mm256_max_ps(_mm256_sub_ps(_mm256_mul_ps(qsat, _mm256_set1_ps(0.70f)), q), zero);
				__m256 cloud_total = _mm256_add_ps(liquid, ice);
				__m256 evap = _mm256_min_ps(cloud_total, _mm256_mul_ps(deficit, _mm256_set1_ps(std::min(cloud_weight * dt / 420.0f, 0.35f))));
				__m256 liquid_share = _mm256_div_ps(liquid, _mm256_max_ps(cloud_total, _mm256_set1_ps(1e-7f)));
				liquid = _mm256_max_ps(_mm256_sub_ps(liquid, _mm256_mul_ps(evap, liquid_share)), zero);
				ice = _mm256_max_ps(_mm256_sub_ps(ice, _mm256_mul_ps(evap, _mm256_sub_ps(_mm256_set1_ps(1.0f), liquid_share))), zero);
				q = _mm256_add_ps(q, evap);
				theta = _mm256_fnmadd_ps(evap, _mm256_set1_ps(2488.0f / sf), theta);

				// Freeze/melt phase conversion.
				__m256 freeze_strength = clamp8(_mm256_mul_ps(_mm256_sub_ps(_mm256_set1_ps(260.0f), temperature), _mm256_set1_ps(1.0f / 14.0f)), 0.0f, 1.0f);
				__m256 freeze = _mm256_mul_ps(liquid, _mm256_mul_ps(freeze_strength, _mm256_set1_ps(std::min(cloud_weight * dt / 900.0f, 0.25f))));
				liquid = _mm256_sub_ps(liquid, freeze);
				ice = _mm256_add_ps(ice, freeze);
				__m256 melt_strength = clamp8(_mm256_mul_ps(_mm256_sub_ps(temperature, _mm256_set1_ps(273.15f)), _mm256_set1_ps(1.0f / 8.0f)), 0.0f, 1.0f);
				__m256 melt = _mm256_mul_ps(ice, _mm256_mul_ps(melt_strength, _mm256_set1_ps(std::min(cloud_weight * dt / 700.0f, 0.30f))));
				ice = _mm256_sub_ps(ice, melt);
				liquid = _mm256_add_ps(liquid, melt);
				// Latent heat of fusion (Lf/cp ~= 333 K per kg/kg) closes the
				// phase-change energy budget.
				theta = _mm256_fmadd_ps(_mm256_sub_ps(freeze, melt), _mm256_set1_ps(333.0f / sf), theta);

				// Autoconversion/fallout. Upper ice falls more slowly than warm rain.
				__m256 rain_excess = _mm256_max_ps(_mm256_sub_ps(liquid, _mm256_set1_ps(0.00010f)), zero);
				__m256 snow_excess = _mm256_max_ps(_mm256_sub_ps(ice, _mm256_set1_ps(0.00007f)), zero);
				__m256 rain_out = _mm256_mul_ps(rain_excess, _mm256_set1_ps(std::min(precipitation_weight * dt / 650.0f, 0.35f)));
				__m256 snow_out = _mm256_mul_ps(snow_excess, _mm256_set1_ps(std::min(precipitation_weight * dt / 1300.0f, 0.25f)));
				liquid = _mm256_sub_ps(liquid, rain_out);
				ice = _mm256_sub_ps(ice, snow_out);
				liquid = _mm256_mul_ps(liquid, _mm256_set1_ps(cloud_decay));
				ice = _mm256_mul_ps(ice, _mm256_set1_ps(cloud_decay));

				__m256 pcol = _mm256_loadu_ps(&a.nprecip[base]);
				__m256 fallout = _mm256_add_ps(rain_out, _mm256_mul_ps(snow_out, _mm256_set1_ps(0.70f)));
				pcol = _mm256_add_ps(pcol, _mm256_mul_ps(fallout, _mm256_set1_ps(3600.0f)));
				_mm256_storeu_ps(&a.nprecip[base], clamp8(pcol, 0.0f, 1.0f));

				_mm256_storeu_ps(&a.ntheta[off + base], clamp8(theta, 220.0f, 430.0f));
				_mm256_storeu_ps(&a.nq[off + base], clamp8(q, 0.00001f, 0.032f));
				_mm256_storeu_ps(&a.nu[off + base], clamp8(u, -110.0f, 110.0f));
				_mm256_storeu_ps(&a.nv[off + base], clamp8(v, -95.0f, 95.0f));
				_mm256_storeu_ps(&a.nliquid[off + base], clamp8(liquid, 0.0f, 0.012f));
				_mm256_storeu_ps(&a.nice[off + base], clamp8(ice, 0.0f, 0.012f));
				_mm256_storeu_ps(&a.npressure[off + base], clamp8(pressure, -6500.0f, 6500.0f));
			}
		}
	}

	if (!is_global) {
		// Complete the local pressure/divergence pair with updated momentum. This
		// is deliberately a second pass: using old pressure and old divergence in
		// the same explicit pass injected energy into the 2.2 km acoustic mode and
		// drove the whole nest to its wind/updraft clamps in under an hour.
		for (int layer = 0; layer < LAYERS; ++layer) {
			int off = a.layer_offset(layer);
			float sigma = SIGMA[layer];
			float coefficient = 8000.0f * sigma * dt * circulation_weight;
			for (int y = 0; y < h; ++y) {
				int yn = std::min(y + 1, h - 1);
				int ys = std::max(y - 1, 0);
				for (int x = 0; x < w; ++x) {
					int xe = std::min(x + 1, w - 1);
					int xw = std::max(x - 1, 0);
					int c = x + y * w;
					float divergence_new = (a.nu[off + xe + y * w] - a.nu[off + xw + y * w])
						* (0.5f / LOCAL_CELL_M)
						+ (a.nv[off + x + yn * w] - a.nv[off + x + ys * w])
						* (0.5f / LOCAL_CELL_M);
					a.npressure[off + c] = std::clamp(
						a.npressure[off + c] - divergence_new * coefficient,
						-6500.0f, 6500.0f);
				}
			}
		}
	}
}


void WeatherNative::surface_pass(Atmosphere &a, SurfaceState &surface, bool is_global, float dt) {
	if (surface.cells != a.cells) return;

	const int l0 = a.layer_offset(0);
	const float sf0 = sigma_temperature_factor(0);
	const float p0_layer = P0 * SIGMA[0];

	#pragma omp parallel for schedule(static)
	for (int c = 0; c < a.cells; ++c) {
		Vector3 d(surface.dir_x[c], surface.dir_y[c], surface.dir_z[c]);
		Vector3 n(surface.normal_x[c], surface.normal_y[c], surface.normal_z[c]);
		if (d.length_squared() < 0.5f) continue;
		d.normalize();
		if (n.length_squared() < 0.5f) n = d;
		else n.normalize();

		float water = std::clamp(surface.water_fraction[c], 0.0f, 1.0f);
		float land = 1.0f - water;
		float moisture = std::clamp(surface.soil_moisture[c], 0.0f, 1.0f);
		float ts = std::clamp(surface.temperature_k[c], 210.0f, 330.0f);
		float tsub = std::clamp(surface.subsurface_temperature_k[c], 210.0f, 330.0f);
		float swe = std::max(surface.snow_swe_kg_m2[c], 0.0f);
		float snow_age = std::max(surface.snow_age_s[c], 0.0f);
		float snow_wet = std::clamp(surface.snow_wetness[c], 0.0f, 1.0f);

		float air_theta = a.ntheta[l0 + c];
		float air_t = std::clamp(air_theta * sf0, 205.0f, 325.0f);
		float air_q = std::clamp(a.nq[l0 + c], 0.00001f, 0.040f);
		float p_abs = std::clamp(p0_layer + a.npressure[l0 + c], 80000.0f, 115000.0f);
		float rho_air = p_abs / std::max(RD_AIR * air_t, 1.0f);
		float wind = std::sqrt(a.nu[l0 + c] * a.nu[l0 + c] + a.nv[l0 + c] * a.nv[l0 + c]);
		// Free convection keeps strongly sun-heated ground coupled to the boundary
		// layer even in light winds; mechanical and buoyant exchange add in
		// quadrature rather than one arbitrarily replacing the other.
		float buoyant_wind = FREE_CONVECTION_WIND_COEFF
			* std::sqrt(std::max(ts - air_t, 0.0f));
		float exchange_wind = std::max(
			std::sqrt(wind * wind + buoyant_wind * buoyant_wind), MIN_EXCHANGE_WIND_MPS);

		// Surface snowfall/rain. The atmospheric precipitation field is an
		// instantaneous intensity diagnostic, mapped here to at most 30 mm/h.
		float precip_mass = std::clamp(a.nprecip[c], 0.0f, 1.0f) * MAX_PRECIP_KG_M2_S;
		float snow_fraction = land * std::clamp((274.15f - air_t) / 4.0f, 0.0f, 1.0f);
		snow_fraction *= std::clamp((275.15f - ts) / 4.0f, 0.0f, 1.0f);
		float snowfall = precip_mass * snow_fraction;
		float rainfall = precip_mass * (1.0f - snow_fraction);
		float fresh_snow = snowfall * dt;
		if (fresh_snow > 1e-6f) {
			swe += fresh_snow;
			// New crystals dominate optical age quickly even under old snow.
			snow_age *= std::exp(-fresh_snow / 2.0f);
		}
		if (land > 0.0f && rainfall > 0.0f) {
			moisture = std::clamp(moisture + rainfall * dt * land / LAND_WATER_RESERVOIR_KG_M2, 0.0f, 1.0f);
		}

		// Dynamic snow albedo: fresh -> aged, then wetness darkens it further.
		float snow_cover = land * (1.0f - std::exp(-swe / SNOW_COVER_EFOLD_KG_M2));
		float age_fraction = 1.0f - std::exp(-snow_age / SNOW_ALBEDO_AGE_TAU_S);
		float dry_snow_albedo = std::lerp(FRESH_SNOW_ALBEDO, AGED_SNOW_ALBEDO, age_fraction);
		float snow_albedo = std::lerp(dry_snow_albedo, WET_SNOW_ALBEDO, snow_wet);
		float snow_free_albedo = std::lerp(
			std::clamp(surface.base_albedo[c], 0.04f, 0.65f), OCEAN_ALBEDO, water);
		float albedo = std::clamp(std::lerp(snow_free_albedo, snow_albedo, snow_cover), 0.03f, 0.94f);

		// Cloud optical attenuation is derived from the 30 prognostic condensate
		// reservoirs, not the renderer's aggregate weather texture.
		float condensate = 0.0f;
		for (int layer = 0; layer < LAYERS; ++layer) {
			int i = a.layer_offset(layer) + c;
			condensate += (a.nliquid[i] + a.nice[i]) * layer_weights[layer];
		}
		condensate *= old_six_level_column_scale();
		float cloud = std::clamp(1.0f - std::exp(-condensate * 1800.0f), 0.0f, 1.0f);

		float sun_dot = d.dot(solar_direction_body);
		float radial_mu = std::max(sun_dot, 0.0f);
		float slope_mu = std::max(n.dot(solar_direction_body), 0.0f);
		float terrain_visibility = 1.0f;
		float sky_view = is_global ? 1.0f : std::clamp(surface.sky_view_factor[c], 0.05f, 1.0f);
		if (!is_global && local_surface_fields_ready && radial_mu > 0.0f) {
			Vector3 tangent_sun = solar_direction_body - d * sun_dot;
			if (tangent_sun.length_squared() > 1e-10f) {
				tangent_sun.normalize();
				float azimuth = std::atan2(tangent_sun.dot(local_north), tangent_sun.dot(local_east));
				if (azimuth < 0.0f) azimuth += TAU_F;
				float sector_f = azimuth / TAU_F * float(HORIZON_SECTORS) - 0.5f;
				int sector0 = int(std::floor(sector_f));
				float blend = sector_f - std::floor(sector_f);
				int s0 = wrap_x(sector0, HORIZON_SECTORS);
				int s1 = wrap_x(sector0 + 1, HORIZON_SECTORS);
				float horizon_tan = std::lerp(
					surface.horizon_tan[size_t(c) * HORIZON_SECTORS + s0],
					surface.horizon_tan[size_t(c) * HORIZON_SECTORS + s1], blend);
				float sun_elevation = std::asin(std::clamp(radial_mu, 0.0f, 1.0f));
				float horizon_angle = std::atan(std::max(horizon_tan, 0.0f));
				float radius = std::max(helion_angular_radius_rad, 0.0001f);
				terrain_visibility = smoothstep01(
					(sun_elevation - (horizon_angle - radius)) / (2.0f * radius));
			}
		}
		surface.terrain_sun_visibility[c] = terrain_visibility;
		float direct_mu = radial_mu > 0.0f ? slope_mu * terrain_visibility : 0.0f;
		float clear_transmission = 0.72f;
		float cloud_direct = std::exp(-2.0f * cloud);
		float direct_sw = solar_irradiance_w_m2 * direct_mu * clear_transmission * cloud_direct;
		float diffuse_sw = solar_irradiance_w_m2 * radial_mu
			* (0.10f + 0.13f * cloud) * (1.0f - 0.35f * cloud) * sky_view;
		float absorbed_sw = std::max((direct_sw + diffuse_sw) * (1.0f - albedo), 0.0f);

		// Net long-wave cooling. Humid/cloudy air is a stronger IR emitter.
		float rh = std::clamp(air_q / std::max(qsat_scalar(air_t, p_abs), 1e-6f), 0.0f, 1.2f);
		float sky_emissivity = std::clamp(0.68f + 0.17f * rh + 0.13f * cloud, 0.60f, 0.98f);
		float surface_emissivity = std::lerp(0.95f, 0.98f, water);
		float lw_up = surface_emissivity * SIGMA_SB * ts * ts * ts * ts;
		float lw_down = sky_emissivity * SIGMA_SB * air_t * air_t * air_t * air_t;
		float net_lw_loss = lw_up - lw_down;

		// Bulk aerodynamic sensible exchange. Positive means surface -> air;
		// increasing wind therefore cools a hot surface and warms a cold one.
		float sensible = rho_air * CP_AIR * BULK_HEAT_COEFF * exchange_wind * (ts - air_t);
		sensible = std::clamp(sensible, -500.0f, 500.0f);

		// Bulk latent exchange. Land evaporation is moisture-limited; open water
		// is effectively unlimited. Negative values are dew/condensation.
		float q_surface_sat = qsat_scalar(ts, P0);
		float humidity_gradient = q_surface_sat - air_q;
		float evap_availability = std::lerp(0.08f + 0.92f * moisture, 1.0f, water);
		if (humidity_gradient < 0.0f) evap_availability = 1.0f;
		float evap_mass_flux = rho_air * BULK_MOISTURE_COEFF * exchange_wind
			* humidity_gradient * evap_availability;
		evap_mass_flux = std::clamp(evap_mass_flux, -0.00020f, 0.00028f);
		float latent = std::clamp(evap_mass_flux * LV_WATER, -500.0f, 700.0f);

		if (land > 0.0f) {
			moisture = std::clamp(
				moisture - evap_mass_flux * dt * land / LAND_WATER_RESERVOIR_KG_M2,
				0.0f, 1.0f);
		}

		// Two thermal reservoirs provide actual inertia. Snow adds heat capacity
		// and sharply reduces conduction to the deeper land reservoir.
		float land_capacity = std::lerp(LAND_DRY_CAPACITY_J_M2_K, LAND_WET_CAPACITY_J_M2_K, moisture);
		float surface_capacity = std::lerp(land_capacity, OCEAN_MIXED_CAPACITY_J_M2_K, water)
			+ swe * CP_SNOW;
		float subsurface_capacity = std::lerp(
			LAND_SUBSURFACE_CAPACITY_J_M2_K, OCEAN_DEEP_CAPACITY_J_M2_K, water);
		float conductance = std::lerp(
			LAND_GROUND_CONDUCTANCE_W_M2_K, OCEAN_DEEP_CONDUCTANCE_W_M2_K, water);
		conductance *= 1.0f / (1.0f + swe * 0.08f * land);
		float ground_flux = conductance * (ts - tsub);

		float net_surface = absorbed_sw - net_lw_loss - sensible - latent - ground_flux;
		float energy = net_surface * dt;

		// A snowpack pins the surface near melting while positive energy is spent
		// on fusion. Meltwater returns to the land moisture reservoir.
		if (swe > 0.0f && ts >= 272.65f && energy > 0.0f) {
			float melt_kg_m2 = std::min(swe, energy / LF_ICE);
			swe -= melt_kg_m2;
			energy -= melt_kg_m2 * LF_ICE;
			if (land > 0.0f) {
				moisture = std::clamp(
					moisture + melt_kg_m2 * land / LAND_WATER_RESERVOIR_KG_M2,
					0.0f, 1.0f);
			}
		}

		ts += energy / std::max(surface_capacity, 1.0f);
		tsub += ground_flux * dt / std::max(subsurface_capacity, 1.0f);
		ts = std::clamp(ts, 210.0f, 330.0f);
		tsub = std::clamp(tsub, 215.0f, 325.0f);

		// Snow optical aging and wetting/refreezing.
		if (swe > 1e-5f) {
			snow_age += dt;
			float melt_wet = std::clamp((ts - 271.5f) / 2.5f, 0.0f, 1.0f);
			float rain_wet = std::clamp(rainfall / std::max(MAX_PRECIP_KG_M2_S * 0.25f, 1e-8f), 0.0f, 1.0f);
			float target_wet = std::max(melt_wet, rain_wet);
			float wet_relax = 1.0f - std::exp(-dt / 1800.0f);
			snow_wet = std::lerp(snow_wet, target_wet, wet_relax);
			if (ts < 270.0f) snow_wet *= std::exp(-dt / 3600.0f);
		} else {
			swe = 0.0f;
			snow_age = 0.0f;
			snow_wet = 0.0f;
		}

		// Return turbulent heat and moisture directly to atmospheric layer 0.
		float air_delta_t = sensible * dt / (BOTTOM_AIR_MASS_KG_M2 * CP_AIR);
		float air_delta_q = evap_mass_flux * dt / BOTTOM_AIR_MASS_KG_M2;
		a.ntheta[l0 + c] = std::clamp(
			a.ntheta[l0 + c] + air_delta_t / sf0, 220.0f, 430.0f);
		a.nq[l0 + c] = std::clamp(a.nq[l0 + c] + air_delta_q, 0.00001f, 0.032f);

		surface.temperature_k[c] = ts;
		surface.subsurface_temperature_k[c] = tsub;
		surface.soil_moisture[c] = moisture;
		surface.snow_swe_kg_m2[c] = swe;
		surface.snow_age_s[c] = snow_age;
		surface.snow_wetness[c] = snow_wet;
		surface.albedo[c] = albedo;
		surface.absorbed_solar_w_m2[c] = absorbed_sw;
		surface.sensible_flux_w_m2[c] = sensible;
		surface.latent_flux_w_m2[c] = latent;
		surface.ground_flux_w_m2[c] = ground_flux;
	}
}

void WeatherNative::vertical_pass(Atmosphere &a, bool is_global, float dt) {
	const __m256 zero = _mm256_setzero_ps();
	const float max_mix = is_global ? 0.085f : 0.12f;
	const float convection_weight = tuning_weights[CONVECTION];

	for (int interface_index = 0; interface_index < INTERFACES; ++interface_index) {
		int lo = interface_index;
		int hi = interface_index + 1;
		int lo_off = a.layer_offset(lo);
		int hi_off = a.layer_offset(hi);
		int flux_off = a.interface_offset(interface_index);

		#pragma omp parallel for schedule(static)
		for (int c = 0; c < a.cells; c += 8) {
			__m256 th_lo = _mm256_loadu_ps(&a.ntheta[lo_off + c]);
			__m256 th_hi = _mm256_loadu_ps(&a.ntheta[hi_off + c]);
			__m256 q_lo = _mm256_loadu_ps(&a.nq[lo_off + c]);
			__m256 q_hi = _mm256_loadu_ps(&a.nq[hi_off + c]);
			__m256 div_lo = _mm256_loadu_ps(&a.divergence[lo_off + c]);
			__m256 precip = _mm256_loadu_ps(&a.nprecip[c]);

			// Approximate moist static instability rather than requiring a dry-theta
			// decrease. Real tropical/severe columns are commonly dry-stable while
			// low-level water vapour makes a lifted parcel strongly buoyant. The
			// 1400 K per kg/kg coefficient is a conservative fraction of Lv/cp.
			__m256 moist_theta_excess = _mm256_add_ps(
				_mm256_sub_ps(th_lo, th_hi),
				_mm256_mul_ps(_mm256_sub_ps(q_lo, q_hi), _mm256_set1_ps(1400.0f)));
			__m256 instability = clamp8(_mm256_mul_ps(
				_mm256_add_ps(moist_theta_excess, _mm256_set1_ps(1.0f)),
				_mm256_set1_ps(1.0f / 18.0f)), 0.0f, 1.0f);
			__m256 moisture = clamp8(_mm256_mul_ps(q_lo, _mm256_set1_ps(1.0f / 0.018f)), 0.0f, 1.0f);
			__m256 convergence_up = _mm256_mul_ps(_mm256_max_ps(_mm256_sub_ps(zero, div_lo), zero), _mm256_set1_ps(0.58f * convection_weight));
			float convective_mass_scale = is_global ? 3.2e-4f : 1.7e-3f;
			__m256 convective_up = _mm256_mul_ps(
				_mm256_mul_ps(instability, moisture),
				_mm256_set1_ps(convective_mass_scale * convection_weight));
			// Positive sensible heat at the lower boundary is an explicit buoyancy
			// source for interface 0. This turns differential slope heating into
			// resolved upslope/thermal circulation instead of relying only on delayed
			// static-instability feedback.
			__m256 surface_buoyancy = zero;
			if (interface_index == 0 && surface_fields_ready) {
				const SurfaceState &surface = is_global ? global_surface : local_surface;
				__m256 sensible = _mm256_loadu_ps(&surface.sensible_flux_w_m2[c]);
				surface_buoyancy = _mm256_mul_ps(
					_mm256_max_ps(sensible, zero),
					_mm256_set1_ps((1.45e-4f / 350.0f) * convection_weight));
			}
			__m256 downdraft = _mm256_mul_ps(precip, _mm256_set1_ps(5.5e-5f));
			__m256 rate = clamp8(_mm256_sub_ps(
				_mm256_add_ps(_mm256_add_ps(convergence_up, convective_up), surface_buoyancy),
				downdraft), -1.8e-4f, 6.0e-4f);
			_mm256_storeu_ps(&a.mass_flux[flux_off + c], rate);

			if (interface_index == 0) {
				// Rain-loaded, evaporatively cooled air produces a shallow cold pool.
				// Its hydrostatic pressure excess and temperature deficit generate the
				// gust-front convergence needed to organize new cells into lines.
				__m256 cold_pool_cooling = _mm256_mul_ps(
					precip, _mm256_set1_ps(dt * 7.0e-4f));
				__m256 cold_pool_pressure = _mm256_mul_ps(
					precip, _mm256_set1_ps(dt * 0.045f));
				th_lo = _mm256_sub_ps(th_lo, cold_pool_cooling);
				_mm256_storeu_ps(&a.ntheta[lo_off + c], clamp8(th_lo, 220.0f, 430.0f));
				__m256 low_pressure = _mm256_loadu_ps(&a.npressure[lo_off + c]);
				_mm256_storeu_ps(&a.npressure[lo_off + c], clamp8(
					_mm256_add_ps(low_pressure, cold_pool_pressure), -6500.0f, 6500.0f));
			}

			__m256 frac = clamp8(_mm256_mul_ps(abs8(rate), _mm256_set1_ps(dt * 0.72f)), 0.0f, max_mix);
			__m256 pos_mask = _mm256_cmp_ps(rate, zero, _CMP_GE_OS);
			__m256 donor_lo_frac = _mm256_blendv_ps(frac, _mm256_mul_ps(frac, _mm256_set1_ps(0.25f)), pos_mask);
			__m256 donor_hi_frac = _mm256_blendv_ps(_mm256_mul_ps(frac, _mm256_set1_ps(0.25f)), frac, pos_mask);

			auto exchange = [&](std::vector<float> &field) {
				__m256 lower = _mm256_loadu_ps(&field[lo_off + c]);
				__m256 upper = _mm256_loadu_ps(&field[hi_off + c]);
				__m256 delta = _mm256_sub_ps(lower, upper);
				lower = _mm256_fnmadd_ps(donor_lo_frac, delta, lower);
				upper = _mm256_fmadd_ps(donor_hi_frac, delta, upper);
				_mm256_storeu_ps(&field[lo_off + c], lower);
				_mm256_storeu_ps(&field[hi_off + c], upper);
			};

			exchange(a.ntheta);
			exchange(a.nq);
			exchange(a.nu);
			exchange(a.nv);
			exchange(a.nliquid);
			exchange(a.nice);
			exchange(a.npressure);

			// Deep ascent preferentially exports condensate upward and gradually
			// glaciates it, producing simulated anvil/upper-cloud reservoirs rather
			// than asking the renderer to invent a tall profile from a storm scalar.
			__m256 ascent = _mm256_max_ps(rate, zero);
			__m256 loft = clamp8(_mm256_mul_ps(ascent, _mm256_set1_ps(dt * 0.42f)), 0.0f, 0.06f);
			__m256 lo_liq = _mm256_loadu_ps(&a.nliquid[lo_off + c]);
			__m256 hi_ice = _mm256_loadu_ps(&a.nice[hi_off + c]);
			__m256 lofted = _mm256_mul_ps(lo_liq, loft);
			lo_liq = _mm256_sub_ps(lo_liq, lofted);
			hi_ice = _mm256_add_ps(hi_ice, _mm256_mul_ps(lofted, _mm256_set1_ps(0.82f)));
			_mm256_storeu_ps(&a.nliquid[lo_off + c], _mm256_max_ps(lo_liq, zero));
			_mm256_storeu_ps(&a.nice[hi_off + c], _mm256_max_ps(hi_ice, zero));
		}
	}

	if (is_global && surface_fields_ready) {
		// A ~21.5 km L0 cell still cannot fully resolve an eyewall. Represent the unresolved
		// wind-induced surface heat exchange / latent-heating feedback only where
		// the resolved environment already supports tropical-cyclone genesis:
		// warm open water, moist ascent, low shear, off-equatorial cyclonic seed.
		// The resulting warm-core pressure tendency is continuous and state based;
		// no storm is spawned by a timer or presentation-layer random event.
		std::array<float, 2> best_activity = {0.0f, 0.0f};
		std::array<int, 2> best_cell = {-1, -1};
		std::fill(tropical_genesis_activity.begin(), tropical_genesis_activity.end(), 0.0f);
		for (int y = 0; y < a.height; ++y) {
			float lat = HALF_PI_F - PI_F * (float(y) + 0.5f) / float(a.height);
			float alat = std::abs(lat);
			if (alat < 5.0f * PI_F / 180.0f || alat > 32.0f * PI_F / 180.0f) continue;
			float hemisphere = lat >= 0.0f ? 1.0f : -1.0f;
			for (int x = 0; x < a.width; ++x) {
				int c = x + y * a.width;
				float water = std::clamp(global_surface.water_fraction[c], 0.0f, 1.0f);
				float sst = global_surface.temperature_k[c];
				if (water < 0.50f || sst < 295.0f) continue;
				int l0 = a.layer_offset(0) + c;
				float ascent = std::max(a.mass_flux[a.interface_offset(0) + c], 0.0f);
				float ascent_factor = 0.18f
					+ 0.82f * smoothstep01((ascent - 1.2e-5f) / 8.0e-5f);
				float moisture_factor = std::clamp(a.nq[l0] / 0.018f, 0.20f, 1.0f);
				float warm_factor = smoothstep01((sst - 295.0f) / 4.0f);
				float max_shear = 0.0f;
				for (int layer = 0; layer < LAYERS; ++layer) {
					max_shear = std::max(max_shear, a.shear[a.layer_offset(layer) + c]);
				}
				// Strong shear suppresses tropical organization but should not make the
				// coarse parameterization discontinuously zero in every six-layer
				// column; retain a small ventilated-convection branch.
				float shear_factor = 0.15f + 0.85f
					* (1.0f - smoothstep01((max_shear - 12.0f) / 22.0f));
				float cyclonic_vorticity = hemisphere * a.vorticity[l0];
				float rotation_factor = smoothstep01((cyclonic_vorticity + 1.5e-5f) / 1.2e-4f);
				float low_pressure_factor = smoothstep01((-a.npressure[l0] + 180.0f) / 1800.0f);
				float activity = warm_factor * moisture_factor * ascent_factor * shear_factor
					* (0.30f + 0.70f * rotation_factor)
					* (0.25f + 0.75f * low_pressure_factor);
				tropical_genesis_activity[c] = activity;
				int hemisphere_index = lat >= 0.0f ? 0 : 1;
				if (activity > best_activity[hemisphere_index]) {
					best_activity[hemisphere_index] = activity;
					best_cell[hemisphere_index] = c;
				}
			}
		}
		const float coarse_cell_ratio = float(GLOBAL_W) / 256.0f;
		// Apply a smooth unresolved eyewall/heating footprint around only the best
		// genesis environment in each hemisphere. This prevents every warm-ocean
		// grid cell from deepening together (which global pressure centering would
		// cancel) while allowing the resolved low to steer and retain the maximum.
		for (int hemisphere_index = 0; hemisphere_index < 2; ++hemisphere_index) {
			int center = tropical_core_cell[hemisphere_index];
			bool retained_existing_core = false;
			if (center >= 0) {
				int old_x = center % a.width;
				int old_y = center / a.width;
				float retained_activity = tropical_genesis_activity[center];
				int retained_cell = center;
				// Advect the sub-grid core only to a clearly better neighbouring
				// environment. Without this memory, the instantaneous global maximum
				// jumps between basins and never lets a closed circulation spin up.
				int track_radius = std::max(2, int(std::round(2.0f * coarse_cell_ratio)));
				for (int oy = -track_radius; oy <= track_radius; ++oy) {
					int yy = std::clamp(old_y + oy, 0, a.height - 1);
					for (int ox = -track_radius; ox <= track_radius; ++ox) {
						int xx = wrap_x(old_x + ox, a.width);
						int candidate = xx + yy * a.width;
						float candidate_activity = tropical_genesis_activity[candidate];
						if (candidate_activity > retained_activity * 1.15f) {
							retained_activity = candidate_activity;
							retained_cell = candidate;
						}
					}
				}
				center = retained_activity >= 5.0e-6f ? retained_cell : -1;
				retained_existing_core = center >= 0;
			}
			if (center < 0 && best_activity[hemisphere_index] >= 1.0e-5f) {
				center = best_cell[hemisphere_index];
			}
			tropical_core_cell[hemisphere_index] = center;
			if (center < 0) {
				tropical_core_age_s[hemisphere_index] = 0.0f;
				continue;
			}
			tropical_core_age_s[hemisphere_index] = retained_existing_core
				? std::min(tropical_core_age_s[hemisphere_index] + dt, 30.0f * 86400.0f)
				: 0.0f;
			float activity = std::max(tropical_genesis_activity[center], 0.0020f);
			int center_x = center % a.width;
			int center_y = center / a.width;
			float core_rate = std::min(activity * 4.0f, 0.055f);
			float hemisphere_sign = hemisphere_index == 0 ? 1.0f : -1.0f;
			int footprint_radius = std::max(3, int(std::round(3.0f * coarse_cell_ratio)));
			for (int oy = -footprint_radius; oy <= footprint_radius; ++oy) {
				int yy = std::clamp(center_y + oy, 0, a.height - 1);
				for (int ox = -footprint_radius; ox <= footprint_radius; ++ox) {
					int xx = wrap_x(center_x + ox, a.width);
					int c = xx + yy * a.width;
					float nx = float(ox) / coarse_cell_ratio;
					float ny = float(oy) / coarse_cell_ratio;
					float radius_sq = nx * nx + ny * ny;
					// Once convection selects an ocean core, its atmospheric warm core is
					// continuous across neighbouring coast/grid-fraction cells. Multiplying
					// every footprint point by its own land mask inverted the pressure
					// gradient near coasts and prevented circulation closure.
					float footprint = std::exp(-radius_sq / 4.5f);
					float pressure_tendency = core_rate * footprint * dt;
					for (int layer = 0; layer < LAYERS && APPROX_HEIGHT_M[layer] <= 3300.0f; ++layer) {
						int i = a.layer_offset(layer) + c;
						float decay = 1.0f - 0.36f * std::clamp(APPROX_HEIGHT_M[layer] / 3300.0f, 0.0f, 1.0f);
						a.npressure[i] = std::clamp(a.npressure[i] - pressure_tendency * decay, -6500.0f, 6500.0f);
					}
					for (int layer = 0; layer < LAYERS && APPROX_HEIGHT_M[layer] <= 5600.0f; ++layer) {
						if (APPROX_HEIGHT_M[layer] < 700.0f) continue;
						int i = a.layer_offset(layer) + c;
						a.ntheta[i] = std::clamp(a.ntheta[i]
							+ activity * footprint * dt * 2.0e-4f, 220.0f, 430.0f);
					}
					// Parameterized unresolved eyewall transport. The coarse cell carries
					// the net angular-momentum convergence and deep mass flux; resolved
					// pressure gradients, advection, drag, landfall, and shear still govern
					// its subsequent evolution.
					float radius = std::sqrt(std::max(radius_sq, 1.0f));
					float eyewall = std::exp(-radius_sq / 6.0f)
						* (1.0f - std::exp(-radius_sq));
					// A newly selected disturbance first has to persist before receiving the
					// full unresolved eyewall closure. This avoids instant hurricane genesis
					// while retaining a weak early circulation that the resolved flow can
					// ventilate or organize.
					float maturity = 0.25f + 0.75f * smoothstep01(
						tropical_core_age_s[hemisphere_index] / (1.5f * 86400.0f));
					float wind_acceleration = std::min(activity * 0.285f, 5.7e-3f)
						* maturity
						* eyewall * dt;
					for (int layer = 0; layer < LAYERS && APPROX_HEIGHT_M[layer] <= 3300.0f; ++layer) {
						int i = a.layer_offset(layer) + c;
						float vertical_decay = 1.0f - 0.44f * std::clamp(APPROX_HEIGHT_M[layer] / 3300.0f, 0.0f, 1.0f);
						a.nu[i] = std::clamp(a.nu[i] + hemisphere_sign * float(oy) / radius
							* wind_acceleration * vertical_decay, -110.0f, 110.0f);
						a.nv[i] = std::clamp(a.nv[i] + hemisphere_sign * float(ox) / radius
							* wind_acceleration * vertical_decay, -95.0f, 95.0f);
					}
					float core_convection = std::min(activity * 0.006f, 2.2e-4f) * footprint;
					for (int interface_index = 0; interface_index < INTERFACES && APPROX_HEIGHT_M[interface_index + 1] <= 5600.0f; ++interface_index) {
						int i = a.interface_offset(interface_index) + c;
						a.mass_flux[i] = std::max(a.mass_flux[i],
							core_convection * (1.0f - 0.42f * std::clamp(APPROX_HEIGHT_M[interface_index + 1] / 5600.0f, 0.0f, 1.0f)));
					}
					int anvil_i = a.layer_offset(nearest_layer_for_height(8500.0f)) + c;
					a.nice[anvil_i] = std::clamp(a.nice[anvil_i]
						+ activity * footprint * dt * 5.0e-8f, 0.0f, 0.012f);
				}
			}
		}
	}

	if (!is_global) {
		// At 2.2 km the nest resolves convective gradients but 30 sigma levels still do
		// not explicitly tilt horizontal vorticity into a mesocyclone. Apply the
		// bounded curl of the resolved updraft gradient in sheared environments;
		// this is the grid-scale analogue of shear tilting and vortex stretching.
		int flux_off = a.interface_offset(0);
		float hemisphere_sign = local_center.y >= 0.0f ? 1.0f : -1.0f;
		for (int y = 1; y < a.height - 1; ++y) {
			for (int x = 1; x < a.width - 1; ++x) {
				int c = x + y * a.width;
				float updraft = std::max(a.mass_flux[flux_off + c], 0.0f);
				if (updraft < 3.5e-5f) continue;
				float gx = (a.mass_flux[flux_off + c + 1]
					- a.mass_flux[flux_off + c - 1]) / 4.0e-4f;
				float gy = (a.mass_flux[flux_off + c + a.width]
					- a.mass_flux[flux_off + c - a.width]) / 4.0e-4f;
				float shear = 0.0f;
				for (int layer = 0; layer < LAYERS && APPROX_HEIGHT_M[layer] <= 3300.0f; ++layer) {
					shear = std::max(shear, a.shear[a.layer_offset(layer) + c]);
				}
				float shear_factor = 0.25f
					+ 0.75f * smoothstep01((shear - 4.0f) / 18.0f);
				float updraft_factor = smoothstep01((updraft - 3.5e-5f) / 1.2e-4f);
				float impulse = dt * 0.0020f * shear_factor * updraft_factor;
				for (int layer = 0; layer < LAYERS && APPROX_HEIGHT_M[layer] <= 3300.0f; ++layer) {
					int i = a.layer_offset(layer) + c;
					float vertical_decay = 1.0f - 0.50f * std::clamp(APPROX_HEIGHT_M[layer] / 3300.0f, 0.0f, 1.0f);
					a.nu[i] = std::clamp(a.nu[i]
						+ hemisphere_sign * gy * impulse * vertical_decay, -110.0f, 110.0f);
					a.nv[i] = std::clamp(a.nv[i]
						- hemisphere_sign * gx * impulse * vertical_decay, -95.0f, 95.0f);
				}
			}
		}
	}

	if (!is_global) {
		nudge_local_boundaries(dt);
	}
}

void WeatherNative::diagnose(Atmosphere &a, bool is_global) {
	const int w = a.width;
	const int h = a.height;
	const float local_lat = std::asin(std::clamp(local_center.y, -1.0f, 1.0f));
	#pragma omp parallel for schedule(static)
	for (int layer = 0; layer < LAYERS; ++layer) {
		alignas(32) int east_idx[8], west_idx[8], north_idx[8], south_idx[8];
		alignas(32) float north_sign[8], south_sign[8];
		int off = a.layer_offset(layer);
		int below_layer = std::max(layer - 1, 0);
		int above_layer = std::min(layer + 1, LAYERS - 1);
		int below_off = a.layer_offset(below_layer);
		int above_off = a.layer_offset(above_layer);
		float dsigma = std::max(std::abs(SIGMA[above_layer] - SIGMA[below_layer]), 0.005f);

		for (int y = 0; y < h; ++y) {
			float lat = is_global ? HALF_PI_F - PI_F * (float(y) + 0.5f) / float(h) : local_lat;
			float coslat = std::cos(lat);
			float dx = is_global ? TAU_F * PLANET_RADIUS_M * coslat / float(w) : LOCAL_CELL_M;
			float dy = is_global ? PI_F * PLANET_RADIUS_M / float(h) : LOCAL_CELL_M;
			float coriolis = 2.0f * ROTATION_RATE * std::sin(lat);
			float curvature = is_global ? std::tan(lat) / PLANET_RADIUS_M : 0.0f;

			for (int x = 0; x < w; x += 8) {
				for (int k = 0; k < 8; ++k) {
					int xx = x + k;
					east_idx[k] = (is_global ? wrap_x(xx + 1, w) : std::min(xx + 1, w - 1)) + y * w;
					west_idx[k] = (is_global ? wrap_x(xx - 1, w) : std::max(xx - 1, 0)) + y * w;
					north_idx[k] = is_global
						? spherical_cell(xx, y - 1, w, h)
						: xx + std::min(y + 1, h - 1) * w;
					south_idx[k] = is_global
						? spherical_cell(xx, y + 1, w, h)
						: xx + std::max(y - 1, 0) * w;
					north_sign[k] = is_global ? spherical_vector_sign(y - 1, h) : 1.0f;
					south_sign[k] = is_global ? spherical_vector_sign(y + 1, h) : 1.0f;
				}
				__m256i ei = _mm256_load_si256(reinterpret_cast<const __m256i *>(east_idx));
				__m256i wi = _mm256_load_si256(reinterpret_cast<const __m256i *>(west_idx));
				__m256i ni = _mm256_load_si256(reinterpret_cast<const __m256i *>(north_idx));
				__m256i si = _mm256_load_si256(reinterpret_cast<const __m256i *>(south_idx));
				const float *u_base = a.u.data() + off;
				const float *v_base = a.v.data() + off;
				__m256 ue = _mm256_i32gather_ps(u_base, ei, 4);
				__m256 uw = _mm256_i32gather_ps(u_base, wi, 4);
				__m256 un = _mm256_i32gather_ps(u_base, ni, 4);
				__m256 us = _mm256_i32gather_ps(u_base, si, 4);
				__m256 ve = _mm256_i32gather_ps(v_base, ei, 4);
				__m256 vw = _mm256_i32gather_ps(v_base, wi, 4);
				__m256 vn = _mm256_i32gather_ps(v_base, ni, 4);
				__m256 vs = _mm256_i32gather_ps(v_base, si, 4);
				__m256 north_sv = _mm256_load_ps(north_sign);
				__m256 south_sv = _mm256_load_ps(south_sign);
				un = _mm256_mul_ps(un, north_sv);
				us = _mm256_mul_ps(us, south_sv);
				vn = _mm256_mul_ps(vn, north_sv);
				vs = _mm256_mul_ps(vs, south_sv);
				int base = x + y * w;
				__m256 center_u = _mm256_loadu_ps(&a.u[off + base]);
				__m256 center_v = _mm256_loadu_ps(&a.v[off + base]);
				__m256 div = _mm256_add_ps(
					_mm256_mul_ps(_mm256_sub_ps(ue, uw), _mm256_set1_ps(0.5f / dx)),
					_mm256_mul_ps(_mm256_sub_ps(vn, vs), _mm256_set1_ps(0.5f / dy)));
				div = _mm256_fnmadd_ps(center_v, _mm256_set1_ps(curvature), div);
				__m256 vort = _mm256_sub_ps(
					_mm256_mul_ps(_mm256_sub_ps(ve, vw), _mm256_set1_ps(0.5f / dx)),
					_mm256_mul_ps(_mm256_sub_ps(un, us), _mm256_set1_ps(0.5f / dy)));
				vort = _mm256_fmadd_ps(center_u, _mm256_set1_ps(curvature), vort);
				_mm256_storeu_ps(&a.divergence[off + base], div);
				_mm256_storeu_ps(&a.vorticity[off + base], vort);

				__m256 theta_below = _mm256_loadu_ps(&a.theta[below_off + base]);
				__m256 theta_above = _mm256_loadu_ps(&a.theta[above_off + base]);
				__m256 stratification = _mm256_mul_ps(_mm256_sub_ps(theta_above, theta_below), _mm256_set1_ps(1.0f / dsigma));
				__m256 pv = _mm256_mul_ps(_mm256_add_ps(vort, _mm256_set1_ps(coriolis)), stratification);
				_mm256_storeu_ps(&a.potential_vorticity[off + base], pv);

				int shear_other = layer < LAYERS - 1 ? above_off : below_off;
				__m256 u_here = _mm256_loadu_ps(&a.u[off + base]);
				__m256 v_here = _mm256_loadu_ps(&a.v[off + base]);
				__m256 du = _mm256_sub_ps(_mm256_loadu_ps(&a.u[shear_other + base]), u_here);
				__m256 dv = _mm256_sub_ps(_mm256_loadu_ps(&a.v[shear_other + base]), v_here);
				__m256 shear = _mm256_sqrt_ps(_mm256_fmadd_ps(dv, dv, _mm256_mul_ps(du, du)));
				_mm256_storeu_ps(&a.shear[off + base], shear);
			}
		}
	}
}

void WeatherNative::update_local_basis(const Vector3 &d) {
	Vector3 center = d.normalized();
	// Parallel-transport the old x axis into the new tangent plane. This avoids
	// the abrupt 180-degree patch flip caused by switching reference axes near a
	// pole while retaining a deterministic fallback for the degenerate case.
	Vector3 transported_east = local_east - center * local_east.dot(center);
	if (transported_east.length_squared() < 1e-8f) {
		Vector3 reference = std::abs(center.y) > 0.92f ? Vector3(1, 0, 0) : Vector3(0, 1, 0);
		transported_east = reference.cross(center);
	}
	local_center = center;
	local_east = transported_east.normalized();
	local_north = local_center.cross(local_east).normalized();
}

void WeatherNative::set_local_center(const Vector3 &d) {
	Vector3 requested = d.normalized();
	if (!local_initialized) {
		update_local_basis(requested);
		initialize_local();
		return;
	}

	// Keep the nest spatially coherent instead of silently rotating its data every
	// frame. Once the player moves ~18% of the nest width, rebuild from the same
	// 30 global sigma levels. A future remapping pass can replace this reset.
	float dotv = std::clamp(local_center.dot(requested), -1.0f, 1.0f);
	float shift_m = std::acos(dotv) * PLANET_RADIUS_M;
	if (shift_m > get_local_span_m() * 0.18f) {
		update_local_basis(requested);
		initialize_local();
	}
}

void WeatherNative::global_state_at_dir_layer(const Vector3 &d, int layer,
	float &theta, float &q, float &u, float &v, float &liquid,
	float &ice, float &pressure) const {
	Vector3 n = d.normalized();
	float lon = std::atan2(n.z, n.x);
	if (lon < 0.0f) lon += TAU_F;
	float lat = std::asin(std::clamp(n.y, -1.0f, 1.0f));
	float x = lon / TAU_F * float(GLOBAL_W) - 0.5f;
	float y = (HALF_PI_F - lat) / PI_F * float(GLOBAL_H) - 0.5f;
	theta = sample_global_layer(global_atm.theta, layer, x, y);
	q = sample_global_layer(global_atm.q, layer, x, y);
	u = sample_global_layer(global_atm.u, layer, x, y);
	v = sample_global_layer(global_atm.v, layer, x, y);
	liquid = sample_global_layer(global_atm.liquid, layer, x, y);
	ice = sample_global_layer(global_atm.ice, layer, x, y);
	pressure = sample_global_layer(global_atm.pressure, layer, x, y);
}

void WeatherNative::rotate_global_wind_to_local(const Vector3 &d, float &u, float &v) const {
	Vector3 n = d.normalized();
	// Global u/v is geographic east/north, whereas the nest evolves components
	// along its fixed tangent-plane x/y axes. Project the physical vector rather
	// than copying components whose bases can rotate through 360 degrees around a
	// polar nest.
	Vector3 global_east(-n.z, 0.0f, n.x);
	if (global_east.length_squared() < 1e-10f) {
		global_east = local_east;
	} else {
		global_east.normalize();
	}
	Vector3 global_north = global_east.cross(n).normalized();
	Vector3 wind = global_east * u + global_north * v;
	u = wind.dot(local_east);
	v = wind.dot(local_north);
}

void WeatherNative::initialize_local() {
	float half = LOCAL_CELL_M * float(LOCAL_W) * 0.5f;
	// Bilinear downscaling from an ~86 km parent grid is otherwise almost
	// perfectly smooth at 2.2 km. Seed a tiny, deterministic boundary-layer
	// perturbation spectrum so explicitly resolved convection can choose cells
	// and lines. The amplitude is well below ordinary observational noise and is
	// tapered out across the boundary-relaxation rim.
	float seed_phase = float(seed & 0xffffu) * 0.000173f;
	for (int y = 0; y < LOCAL_H; ++y) {
		for (int x = 0; x < LOCAL_W; ++x) {
			float ex = (float(x) + 0.5f) * LOCAL_CELL_M - half;
			float ny = (float(y) + 0.5f) * LOCAL_CELL_M - half;
			Vector3 d = (local_center + local_east * (ex / PLANET_RADIUS_M)
				+ local_north * (ny / PLANET_RADIUS_M)).normalized();
			int c = x + y * LOCAL_W;
			int edge_dist = std::min(std::min(x, LOCAL_W - 1 - x), std::min(y, LOCAL_H - 1 - y));
			float interior = smoothstep01(float(edge_dist) / 24.0f);
			float perturbation = interior * (
				0.58f * std::sin(float(x) * 0.31f + seed_phase) * std::sin(float(y) * 0.27f - seed_phase * 0.7f)
				+ 0.29f * std::sin(float(x + y) * 0.113f + seed_phase * 1.9f)
				+ 0.13f * std::cos(float(2 * x - y) * 0.071f - seed_phase));
			for (int layer = 0; layer < LAYERS; ++layer) {
				float theta, q, u, v, liquid, ice, pressure;
				global_state_at_dir_layer(d, layer, theta, q, u, v, liquid, ice, pressure);
				rotate_global_wind_to_local(d, u, v);
				int i = local_atm.layer_offset(layer) + c;
				float low_level = 1.0f - float(layer) / float(LAYERS - 1);
				local_atm.theta[i] = theta + perturbation * 0.12f * low_level;
				local_atm.q[i] = std::clamp(q + perturbation * 3.0e-5f * low_level, 0.00001f, 0.032f);
				local_atm.u[i] = u;
				local_atm.v[i] = v;
				local_atm.liquid[i] = liquid;
				local_atm.ice[i] = ice;
				local_atm.pressure[i] = pressure + perturbation * 3.0f * low_level;
			}
			local_atm.precip[c] = 0.0f;
		}
	}
	if (surface_fields_ready) initialize_local_surface();
	local_initialized = true;
	diagnose(local_atm, false);
}


void WeatherNative::initialize_local_surface() {
	float half = LOCAL_CELL_M * float(LOCAL_W) * 0.5f;
	for (int y = 0; y < LOCAL_H; ++y) {
		for (int x = 0; x < LOCAL_W; ++x) {
			float ex = (float(x) + 0.5f) * LOCAL_CELL_M - half;
			float ny = (float(y) + 0.5f) * LOCAL_CELL_M - half;
			Vector3 d = (local_center + local_east * (ex / PLANET_RADIUS_M)
				+ local_north * (ny / PLANET_RADIUS_M)).normalized();
			int c = x + y * LOCAL_W;
			local_surface.dir_x[c] = d.x;
			local_surface.dir_y[c] = d.y;
			local_surface.dir_z[c] = d.z;
			local_surface.elevation_m[c] = sample_global_surface_scalar(global_surface.elevation_m, d);
			local_surface.water_fraction[c] = std::clamp(
				sample_global_surface_scalar(global_surface.water_fraction, d), 0.0f, 1.0f);
			local_surface.soil_moisture[c] = std::clamp(
				sample_global_surface_scalar(global_surface.soil_moisture, d), 0.0f, 1.0f);
			local_surface.base_albedo[c] = std::clamp(
				sample_global_surface_scalar(global_surface.base_albedo, d), 0.04f, 0.65f);
			Vector3 n(
				sample_global_surface_scalar(global_surface.normal_x, d),
				sample_global_surface_scalar(global_surface.normal_y, d),
				sample_global_surface_scalar(global_surface.normal_z, d));
			if (n.length_squared() < 1e-8f) n = d;
			else n.normalize();
			local_surface.normal_x[c] = n.x;
			local_surface.normal_y[c] = n.y;
			local_surface.normal_z[c] = n.z;
			local_surface.temperature_k[c] = sample_global_surface_scalar(global_surface.temperature_k, d);
			local_surface.subsurface_temperature_k[c] = sample_global_surface_scalar(global_surface.subsurface_temperature_k, d);
			local_surface.snow_swe_kg_m2[c] = std::max(
				sample_global_surface_scalar(global_surface.snow_swe_kg_m2, d), 0.0f);
			local_surface.snow_age_s[c] = std::max(
				sample_global_surface_scalar(global_surface.snow_age_s, d), 0.0f);
			local_surface.snow_wetness[c] = std::clamp(
				sample_global_surface_scalar(global_surface.snow_wetness, d), 0.0f, 1.0f);
			local_surface.albedo[c] = std::clamp(
				sample_global_surface_scalar(global_surface.albedo, d), 0.03f, 0.94f);
			local_surface.absorbed_solar_w_m2[c] = 0.0f;
			local_surface.sensible_flux_w_m2[c] = 0.0f;
			local_surface.latent_flux_w_m2[c] = 0.0f;
			local_surface.ground_flux_w_m2[c] = 0.0f;
			local_surface.sky_view_factor[c] = 1.0f;
			local_surface.terrain_sun_visibility[c] = 1.0f;
			for (int sector = 0; sector < HORIZON_SECTORS; ++sector) {
				local_surface.horizon_tan[size_t(c) * HORIZON_SECTORS + sector] = 0.0f;
			}
		}
	}
	local_surface_fields_ready = false;
}

void WeatherNative::update_local_surface_geometry() {
	float half = LOCAL_CELL_M * float(LOCAL_W) * 0.5f;
	for (int y = 0; y < LOCAL_H; ++y) {
		for (int x = 0; x < LOCAL_W; ++x) {
			int c = x + y * LOCAL_W;
			float ex = (float(x) + 0.5f) * LOCAL_CELL_M - half;
			float ny = (float(y) + 0.5f) * LOCAL_CELL_M - half;
			Vector3 d = (local_center + local_east * (ex / PLANET_RADIUS_M)
				+ local_north * (ny / PLANET_RADIUS_M)).normalized();
			local_surface.dir_x[c] = d.x;
			local_surface.dir_y[c] = d.y;
			local_surface.dir_z[c] = d.z;

			Vector3 supplied(local_surface.normal_x[c], local_surface.normal_y[c], local_surface.normal_z[c]);
			if (supplied.length_squared() < 0.25f) {
				int xe = std::min(x + 1, LOCAL_W - 1);
				int xw = std::max(x - 1, 0);
				int yn = std::min(y + 1, LOCAL_H - 1);
				int ys = std::max(y - 1, 0);
				auto position_at = [&](int sx, int sy) {
					int sc = sx + sy * LOCAL_W;
					float sex = (float(sx) + 0.5f) * LOCAL_CELL_M - half;
					float sny = (float(sy) + 0.5f) * LOCAL_CELL_M - half;
					Vector3 sd = (local_center + local_east * (sex / PLANET_RADIUS_M)
						+ local_north * (sny / PLANET_RADIUS_M)).normalized();
					float water = std::clamp(local_surface.water_fraction[sc], 0.0f, 1.0f);
					float h = local_surface.elevation_m[sc];
					return sd * (PLANET_RADIUS_M + h);
				};
				Vector3 tangent_x = position_at(xe, y) - position_at(xw, y);
				Vector3 tangent_y = position_at(x, yn) - position_at(x, ys);
				supplied = tangent_x.cross(tangent_y);
				if (supplied.length_squared() < 1e-10f) supplied = d;
				else supplied.normalize();
			}
			if (supplied.dot(d) < 0.0f) supplied = -supplied;
			float water = std::clamp(local_surface.water_fraction[c], 0.0f, 1.0f);
			Vector3 n = supplied.lerp(d, water).normalized();
			local_surface.normal_x[c] = n.x;
			local_surface.normal_y[c] = n.y;
			local_surface.normal_z[c] = n.z;
		}
	}
}

void WeatherNative::update_local_surface_horizon() {
	for (int y = 0; y < LOCAL_H; ++y) {
		for (int x = 0; x < LOCAL_W; ++x) {
			int c = x + y * LOCAL_W;
			Vector3 d0(local_surface.dir_x[c], local_surface.dir_y[c], local_surface.dir_z[c]);
			d0.normalize();
			float water0 = std::clamp(local_surface.water_fraction[c], 0.0f, 1.0f);
			float h0 = local_surface.elevation_m[c];
			Vector3 p0 = d0 * (PLANET_RADIUS_M + h0);
			float sky_sum = 0.0f;
			for (int sector = 0; sector < HORIZON_SECTORS; ++sector) {
				float angle = (float(sector) + 0.5f) * TAU_F / float(HORIZON_SECTORS);
				float sx_dir = std::cos(angle);
				float sy_dir = std::sin(angle);
				float max_tan = 0.0f;
				for (int step : HORIZON_MARCH_CELLS) {
					int sx = int(std::round(float(x) + sx_dir * float(step)));
					int sy = int(std::round(float(y) + sy_dir * float(step)));
					if (sx < 0 || sx >= LOCAL_W || sy < 0 || sy >= LOCAL_H) break;
					int sc = sx + sy * LOCAL_W;
					Vector3 d1(local_surface.dir_x[sc], local_surface.dir_y[sc], local_surface.dir_z[sc]);
					d1.normalize();
					float water1 = std::clamp(local_surface.water_fraction[sc], 0.0f, 1.0f);
					float h1 = local_surface.elevation_m[sc];
					Vector3 rel = d1 * (PLANET_RADIUS_M + h1) - p0;
					float vertical = rel.dot(d0);
					float horizontal_sq = std::max(rel.length_squared() - vertical * vertical, 1.0f);
					float horizon_tan = vertical / std::sqrt(horizontal_sq);
					max_tan = std::max(max_tan, horizon_tan);
				}
				max_tan = std::max(max_tan, 0.0f);
				local_surface.horizon_tan[size_t(c) * HORIZON_SECTORS + sector] = max_tan;
				// Isotropic sky-view approximation: cos^2(horizon elevation), averaged
				// over azimuth. It attenuates diffuse short-wave and leaves open terrain 1.
				sky_sum += 1.0f / (1.0f + max_tan * max_tan);
			}
			local_surface.sky_view_factor[c] = std::clamp(
				sky_sum / float(HORIZON_SECTORS), 0.05f, 1.0f);
		}
	}
}

void WeatherNative::set_local_surface_fields(const PackedFloat32Array &fields) {
	constexpr int STRIDE = 7;
	if (fields.size() != LOCAL_W * LOCAL_H * STRIDE || !local_initialized) return;
	for (int c = 0; c < local_surface.cells; ++c) {
		local_surface.elevation_m[c] = fields[c * STRIDE + 0];
		local_surface.water_fraction[c] = std::clamp(fields[c * STRIDE + 1], 0.0f, 1.0f);
		local_surface.soil_moisture[c] = std::clamp(fields[c * STRIDE + 2], 0.0f, 1.0f);
		local_surface.base_albedo[c] = std::clamp(fields[c * STRIDE + 3], 0.04f, 0.65f);
		local_surface.normal_x[c] = fields[c * STRIDE + 4];
		local_surface.normal_y[c] = fields[c * STRIDE + 5];
		local_surface.normal_z[c] = fields[c * STRIDE + 6];
	}
	update_local_surface_geometry();
	update_local_surface_horizon();
	local_surface_fields_ready = true;
}

void WeatherNative::nudge_local_boundaries(float dt) {
	const int rim = 24;
	const float half = LOCAL_CELL_M * float(LOCAL_W) * 0.5f;
	for (int y = 0; y < LOCAL_H; ++y) {
		for (int x = 0; x < LOCAL_W; ++x) {
			int edge_dist = std::min(std::min(x, LOCAL_W - 1 - x), std::min(y, LOCAL_H - 1 - y));
			if (edge_dist >= rim) continue;
			float edge = 1.0f - float(edge_dist) / float(rim);
			float weight = edge * std::min(dt / 110.0f, 0.28f);
			float ex = (float(x) + 0.5f) * LOCAL_CELL_M - half;
			float ny = (float(y) + 0.5f) * LOCAL_CELL_M - half;
			Vector3 d = (local_center + local_east * (ex / PLANET_RADIUS_M)
				+ local_north * (ny / PLANET_RADIUS_M)).normalized();
			int c = x + y * LOCAL_W;
			for (int layer = 0; layer < LAYERS; ++layer) {
				float theta, q, u, v, liquid, ice, pressure;
				global_state_at_dir_layer(d, layer, theta, q, u, v, liquid, ice, pressure);
				rotate_global_wind_to_local(d, u, v);
				int i = local_atm.layer_offset(layer) + c;
				local_atm.ntheta[i] = std::lerp(local_atm.ntheta[i], theta, weight);
				local_atm.nq[i] = std::lerp(local_atm.nq[i], q, weight);
				local_atm.nu[i] = std::lerp(local_atm.nu[i], u, weight);
				local_atm.nv[i] = std::lerp(local_atm.nv[i], v, weight);
				local_atm.nliquid[i] = std::lerp(local_atm.nliquid[i], liquid, weight * 0.8f);
				local_atm.nice[i] = std::lerp(local_atm.nice[i], ice, weight * 0.8f);
				local_atm.npressure[i] = std::lerp(local_atm.npressure[i], pressure, weight);
			}
		}
	}
}

void WeatherNative::step_local(float dt) {
	if (!local_initialized) initialize_local();
	dt = std::clamp(dt, 1.0f, 40.0f);
	// The 2.2 km pressure/divergence mode has a much tighter CFL limit than the
	// global mesh. Public 20/40 s cadence is retained for callers, but integration
	// is substepped so a nest cannot turn into a domain-wide acoustic/convective
	// runaway before boundaries can respond.
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

void WeatherNative::reset_local_from_global() {
	initialize_local();
}

static PackedFloat32Array weather_rgba_from(const WeatherNative::Atmosphere &a,
		const std::array<float, WeatherNative::LAYERS> &layer_weights, bool is_global) {
	PackedFloat32Array out;
	out.resize(a.cells * 4);
	float *w = out.ptrw();
	#pragma omp parallel for schedule(static)
	for (int c = 0; c < a.cells; ++c) {
		float condensate = 0.0f;
		float max_ascent = 0.0f;
		float max_rotation = 0.0f;
		float max_shear = 0.0f;
		for (int layer = 0; layer < WeatherNative::LAYERS; ++layer) {
			int i = a.layer_offset(layer) + c;
			condensate += (a.liquid[i] + a.ice[i]) * layer_weights[layer];
			if (WeatherNative::APPROX_HEIGHT_M[layer] <= 5600.0f) max_rotation = std::max(max_rotation, std::abs(a.vorticity[i]));
			max_shear = std::max(max_shear, a.shear[i]);
		}
		condensate *= old_six_level_column_scale();
		for (int interface_index = 0; interface_index < WeatherNative::INTERFACES; ++interface_index) {
			max_ascent = std::max(max_ascent, a.mass_flux[a.interface_offset(interface_index) + c]);
		}
		int l0 = a.layer_offset(0) + c;
		int l2 = a.layer_offset(nearest_layer_for_height(3300.0f)) + c;
		float moist_theta_excess = a.theta[l0] - a.theta[l2]
			+ 1400.0f * (a.q[l0] - a.q[l2]);
		float instability = std::clamp((moist_theta_excess + 1.0f) / 18.0f, 0.0f, 1.0f);
		float moisture = std::clamp(a.q[l0] / 0.018f, 0.0f, 1.0f);
		float ascent = std::clamp(max_ascent / 2.5e-4f, 0.0f, 1.0f);
		float rotation = std::clamp(max_rotation / 0.00030f, 0.0f, 1.0f);
		float shear = std::clamp(max_shear / 30.0f, 0.0f, 1.0f);
		float polar_resolution = 1.0f;
		if (is_global) {
			int y = c / a.width;
			float lat = HALF_PI_F - PI_F * (float(y) + 0.5f) / float(a.height);
			polar_resolution = smoothstep01(std::cos(lat) / 0.20f);
		}
		float storm = std::clamp(polar_resolution
			* (0.36f * ascent + 0.24f * instability + 0.20f * rotation + 0.20f * shear)
			* (0.35f + 0.65f * moisture), 0.0f, 1.0f);
		// Exponential random-overlap closure: the calibrated coefficient gives a
		// roughly 60% global cloud fraction after spin-up, close to Earth's
		// observed whole-sky climatology without forcing every column overcast.
		float cloud = std::clamp(1.0f - std::exp(-condensate * 4000.0f) + storm * 0.14f, 0.0f, 1.0f);
		w[c * 4 + 0] = cloud;
		w[c * 4 + 1] = storm;
		w[c * 4 + 2] = std::clamp(a.precip[c], 0.0f, 1.0f);
		w[c * 4 + 3] = std::clamp(0.5f + a.pressure[l0] / 12000.0f, 0.0f, 1.0f);
	}
	return out;
}

static PackedFloat32Array diagnostics_rgba_from(const WeatherNative::Atmosphere &a, bool is_global) {
	PackedFloat32Array out;
	out.resize(a.cells * 4);
	float *w = out.ptrw();
	#pragma omp parallel for schedule(static)
	for (int c = 0; c < a.cells; ++c) {
		float polar_resolution = 1.0f;
		if (is_global) {
			int y = c / a.width;
			float lat = HALF_PI_F - PI_F * (float(y) + 0.5f) / float(a.height);
			polar_resolution = smoothstep01(std::cos(lat) / 0.20f);
		}
		// Select the strongest signed low/mid-level rotation so cyclonic and
		// anticyclonic structures remain visible instead of cancelling vertically.
		float vort = 0.0f;
		for (int layer = 0; layer < WeatherNative::LAYERS && WeatherNative::APPROX_HEIGHT_M[layer] <= 5600.0f; ++layer) {
			float z = a.vorticity[a.layer_offset(layer) + c];
			if (std::abs(z) > std::abs(vort)) vort = z;
		}
		float div = a.divergence[a.layer_offset(nearest_layer_for_height(1700.0f)) + c];
		float pv = a.potential_vorticity[a.layer_offset(nearest_layer_for_height(8500.0f)) + c];
		float shear = 0.0f;
		for (int layer = 0; layer < WeatherNative::LAYERS; ++layer) {
			shear = std::max(shear, a.shear[a.layer_offset(layer) + c]);
		}
		w[c * 4 + 0] = std::clamp(0.5f + polar_resolution * vort / 0.0008f, 0.0f, 1.0f);
		w[c * 4 + 1] = std::clamp(0.5f + polar_resolution * div / 0.0006f, 0.0f, 1.0f);
		w[c * 4 + 2] = std::clamp(0.5f + polar_resolution * pv / 0.0030f, 0.0f, 1.0f);
		w[c * 4 + 3] = std::clamp(shear / 35.0f, 0.0f, 1.0f);
	}
	return out;
}

PackedFloat32Array WeatherNative::get_global_weather_rgba() const {
	return weather_rgba_from(global_atm, layer_weights, true);
}

PackedFloat32Array WeatherNative::get_local_weather_rgba() const {
	return weather_rgba_from(local_atm, layer_weights, false);
}

PackedFloat32Array WeatherNative::get_global_diagnostics_rgba() const {
	return diagnostics_rgba_from(global_atm, true);
}

PackedFloat32Array WeatherNative::get_local_diagnostics_rgba() const {
	return diagnostics_rgba_from(local_atm, false);
}

static PackedFloat32Array convective_rgba_from(const WeatherNative::Atmosphere &a, bool is_global) {
	PackedFloat32Array out;
	out.resize(a.cells * 4);
	float *w = out.ptrw();
	#pragma omp parallel for schedule(static)
	for (int c = 0; c < a.cells; ++c) {
		float max_ascent = 0.0f;
		float max_downdraft = 0.0f;
		for (int interface_index = 0; interface_index < WeatherNative::INTERFACES; ++interface_index) {
			float flux = a.mass_flux[a.interface_offset(interface_index) + c];
			max_ascent = std::max(max_ascent, flux);
			max_downdraft = std::max(max_downdraft, -flux);
		}
		float upper_ice = 0.0f;
		for (int layer = 0; layer < WeatherNative::LAYERS; ++layer) {
			int i = a.layer_offset(layer) + c;
			if (WeatherNative::APPROX_HEIGHT_M[layer] >= 5600.0f) upper_ice += a.ice[i] * WeatherNative::DEFAULT_LAYER_WEIGHTS[layer];
		}
		upper_ice *= old_six_level_column_scale();
		float polar_resolution = 1.0f;
		if (is_global) {
			int y = c / a.width;
			float lat = HALF_PI_F - PI_F * (float(y) + 0.5f) / float(a.height);
			polar_resolution = smoothstep01(std::cos(lat) / 0.20f);
		}
		float ascent = std::clamp(max_ascent / 4.0e-4f, 0.0f, 1.0f);
		int l0 = a.layer_offset(0) + c;
		float surface_wind = std::sqrt(a.u[l0] * a.u[l0] + a.v[l0] * a.v[l0]);
		w[c * 4 + 0] = ascent;
		w[c * 4 + 1] = std::clamp(max_downdraft / 1.5e-4f, 0.0f, 1.0f);
		w[c * 4 + 2] = std::clamp(1.0f - std::exp(-upper_ice * 6000.0f), 0.0f, 1.0f);
		w[c * 4 + 3] = std::clamp(polar_resolution * surface_wind / 60.0f, 0.0f, 1.0f);
	}
	return out;
}

PackedFloat32Array WeatherNative::get_global_convective_rgba() const {
	return convective_rgba_from(global_atm, true);
}

PackedFloat32Array WeatherNative::get_local_convective_rgba() const {
	return convective_rgba_from(local_atm, false);
}

PackedFloat32Array WeatherNative::get_tropical_core_diagnostics() const {
	PackedFloat32Array out;
	out.resize(4);
	for (int hemisphere = 0; hemisphere < 2; ++hemisphere) {
		int cell = tropical_core_cell[hemisphere];
		out[hemisphere] = float(cell);
		out[2 + hemisphere] = cell >= 0 && cell < int(tropical_genesis_activity.size())
			? tropical_genesis_activity[cell] : 0.0f;
	}
	return out;
}

static PackedFloat32Array surface_rgba_from(const WeatherNative::SurfaceState &surface) {
	PackedFloat32Array out;
	out.resize(surface.cells * 4);
	float *w = out.ptrw();
	#pragma omp parallel for schedule(static)
	for (int c = 0; c < surface.cells; ++c) {
		float snow_cover = (1.0f - std::clamp(surface.water_fraction[c], 0.0f, 1.0f))
			* (1.0f - std::exp(-std::max(surface.snow_swe_kg_m2[c], 0.0f) / SNOW_COVER_EFOLD_KG_M2));
		w[c * 4 + 0] = std::clamp((surface.temperature_k[c] - 220.0f) / 110.0f, 0.0f, 1.0f);
		w[c * 4 + 1] = std::clamp(snow_cover, 0.0f, 1.0f);
		w[c * 4 + 2] = std::clamp(surface.albedo[c], 0.0f, 1.0f);
		w[c * 4 + 3] = std::clamp(std::lerp(
			surface.soil_moisture[c], 1.0f, surface.water_fraction[c]), 0.0f, 1.0f);
	}
	return out;
}

PackedFloat32Array WeatherNative::get_global_surface_rgba() const {
	return surface_rgba_from(global_surface);
}

PackedFloat32Array WeatherNative::get_local_surface_rgba() const {
	return surface_rgba_from(local_surface);
}

static PackedFloat32Array products_rgba_from(const WeatherNative::Atmosphere &atmosphere,
		const WeatherNative::SurfaceState &surface) {
	PackedFloat32Array out;
	out.resize(atmosphere.cells * 4);
	float *w = out.ptrw();
	#pragma omp parallel for schedule(static)
	for (int c = 0; c < atmosphere.cells; ++c) {
		int l0 = atmosphere.layer_offset(0) + c;
		int l2 = atmosphere.layer_offset(nearest_layer_for_height(3300.0f)) + c;
		float air_temperature = atmosphere.theta[l0] * sigma_temperature_factor(0);
		float moist_theta_excess = atmosphere.theta[l0] - atmosphere.theta[l2]
			+ 1400.0f * (atmosphere.q[l0] - atmosphere.q[l2]);
		float instability = std::clamp(
			(moist_theta_excess + 1.0f) / 18.0f, 0.0f, 1.0f);
		float moisture = std::clamp(atmosphere.q[l0] / 0.018f, 0.0f, 1.0f);
		float cape_j_kg = 4000.0f * instability * moisture;
		w[c * 4 + 0] = std::clamp((air_temperature - 220.0f) / 100.0f, 0.0f, 1.0f);
		w[c * 4 + 1] = surface.water_fraction[c] >= 0.5f
			? std::clamp((surface.temperature_k[c] - 260.0f) / 45.0f, 0.0f, 1.0f)
			: -1.0f;
		w[c * 4 + 2] = std::clamp(cape_j_kg / 4000.0f, 0.0f, 1.0f);
		w[c * 4 + 3] = std::clamp(surface.absorbed_solar_w_m2[c] / 1200.0f, 0.0f, 1.0f);
	}
	return out;
}

PackedFloat32Array WeatherNative::get_global_products_rgba() const {
	return products_rgba_from(global_atm, global_surface);
}

PackedFloat32Array WeatherNative::get_local_products_rgba() const {
	return products_rgba_from(local_atm, local_surface);
}

} // namespace godot

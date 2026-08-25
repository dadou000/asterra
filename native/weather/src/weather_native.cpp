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
static constexpr float ROTATION_RATE = 7.2921159e-5f;
static constexpr float P0 = 100000.0f;

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

int WeatherNative::wrap_x(int x, int w) {
	x %= w;
	return x < 0 ? x + w : x;
}

int WeatherNative::clamp_y(int y, int h) {
	return std::max(0, std::min(h - 1, y));
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
}

void WeatherNative::_bind_methods() {
	ClassDB::bind_method(D_METHOD("initialize", "seed"), &WeatherNative::initialize);
	ClassDB::bind_method(D_METHOD("step_global", "dt"), &WeatherNative::step_global);
	ClassDB::bind_method(D_METHOD("set_local_center", "center_dir"), &WeatherNative::set_local_center);
	ClassDB::bind_method(D_METHOD("step_local", "dt"), &WeatherNative::step_local);
	ClassDB::bind_method(D_METHOD("get_global_weather_rgba"), &WeatherNative::get_global_weather_rgba);
	ClassDB::bind_method(D_METHOD("get_local_weather_rgba"), &WeatherNative::get_local_weather_rgba);
	ClassDB::bind_method(D_METHOD("get_global_diagnostics_rgba"), &WeatherNative::get_global_diagnostics_rgba);
	ClassDB::bind_method(D_METHOD("get_local_diagnostics_rgba"), &WeatherNative::get_local_diagnostics_rgba);
	ClassDB::bind_method(D_METHOD("get_layer_count"), &WeatherNative::get_layer_count);
	ClassDB::bind_method(D_METHOD("get_local_center"), &WeatherNative::get_local_center);
	ClassDB::bind_method(D_METHOD("get_local_east"), &WeatherNative::get_local_east);
	ClassDB::bind_method(D_METHOD("get_local_north"), &WeatherNative::get_local_north);
	ClassDB::bind_method(D_METHOD("get_local_span_m"), &WeatherNative::get_local_span_m);
}

float WeatherNative::clamp01(float x) {
	return std::clamp(x, 0.0f, 1.0f);
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
	initialize_global();
	diagnose(global_atm, true);
	local_initialized = false;
}

void WeatherNative::initialize_global() {
	const float phase = float(seed & 0xffffu) * 0.000173f;
	for (int y = 0; y < GLOBAL_H; ++y) {
		float lat = HALF_PI_F - PI_F * (float(y) + 0.5f) / float(GLOBAL_H);
		float alat = std::abs(lat);
		float sinlat = std::sin(alat);
		float surface_t = 302.0f - 48.0f * std::pow(sinlat, 1.35f);
		float surface_q = 0.0185f - 0.0125f * std::pow(sinlat, 1.2f);

		for (int x = 0; x < GLOBAL_W; ++x) {
			int c = x + y * GLOBAL_W;
			float lon = TAU_F * (float(x) + 0.5f) / float(GLOBAL_W);
			float wave = 0.55f * std::sin(lon * 3.0f + lat * 1.7f + phase)
				+ 0.28f * std::sin(lon * 7.0f - lat * 2.3f + phase * 0.31f);
			float syn = std::sin(lon * 2.0f + lat * 4.0f + phase * 3.1f)
				+ 0.35f * std::sin(lon * 5.0f - lat * 1.3f);

			for (int layer = 0; layer < LAYERS; ++layer) {
				int i = global_atm.layer_offset(layer) + c;
				float h = APPROX_HEIGHT_M[layer];
				float sf = sigma_temperature_factor(layer);
				float actual_t = std::clamp(surface_t - 0.00615f * h + wave * (2.1f - 0.20f * layer), 205.0f, 310.0f);
				global_atm.theta[i] = actual_t / sf;
				global_atm.q[i] = std::clamp(surface_q * std::exp(-h / 2450.0f)
					* (1.0f + wave * 0.10f) + 0.00008f, 0.00003f, 0.024f);

				float upper = float(layer) / float(LAYERS - 1);
				float jet = 7.0f + 34.0f * upper;
				jet *= std::exp(-std::pow((alat - 0.73f) / 0.27f, 2.0f));
				float trade = -9.0f * (1.0f - upper * 0.55f)
					* std::exp(-std::pow((alat - 0.30f) / 0.25f, 2.0f));
				global_atm.u[i] = jet + trade + wave * (3.0f + upper * 3.5f);
				global_atm.v[i] = (2.0f + upper * 2.0f) * std::cos(lon * 2.0f + lat * 3.0f + layer * 0.37f);
				global_atm.pressure[i] = syn * 850.0f * (0.95f - upper * 0.35f) + wave * 260.0f;

				float p_abs = P0 * SIGMA[layer] + global_atm.pressure[i];
				float sat = qsat_scalar(actual_t, p_abs);
				float supersat = std::max(global_atm.q[i] - sat * 0.94f, 0.0f);
				float ice_frac = clamp01((268.0f - actual_t) / 20.0f);
				global_atm.liquid[i] = supersat * (1.0f - ice_frac) * 0.55f;
				global_atm.ice[i] = supersat * ice_frac * 0.55f;
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
	float a00 = field[off + global_cell(x0, y0)];
	float a10 = field[off + global_cell(x0 + 1, y0)];
	float a01 = field[off + global_cell(x0, y0 + 1)];
	float a11 = field[off + global_cell(x0 + 1, y0 + 1)];
	return std::lerp(std::lerp(a00, a10, fx), std::lerp(a01, a11, fx), fy);
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
	vertical_pass(global_atm, true, dt);
	swap_state(global_atm);
	diagnose(global_atm, true);
}

void WeatherNative::horizontal_pass(Atmosphere &a, bool is_global, float dt) {
	const __m256 zero = _mm256_setzero_ps();
	const int w = a.width;
	const int h = a.height;
	const float local_lat = std::asin(std::clamp(local_center.y, -1.0f, 1.0f));

	// Precipitation is a column field. It decays between fallout pulses so the
	// output represents current intensity rather than accumulated rainfall.
	float precip_memory = std::exp(-dt / 900.0f);
	for (int i = 0; i < a.cells; i += 8) {
		__m256 p = _mm256_loadu_ps(&a.precip[i]);
		_mm256_storeu_ps(&a.nprecip[i], _mm256_mul_ps(p, _mm256_set1_ps(precip_memory)));
	}

	alignas(32) int adv_idx[8], east_idx[8], west_idx[8], north_idx[8], south_idx[8];

	for (int layer = 0; layer < LAYERS; ++layer) {
		const int off = a.layer_offset(layer);
		const float sigma = SIGMA[layer];
		const float sf = sigma_temperature_factor(layer);
		const float height_m = APPROX_HEIGHT_M[layer];
		const float rho = 1.225f * std::pow(sigma, 0.82f);
		const float wind_tau = 15000.0f + float(layer) * 6500.0f;
		const float wind_damp = std::exp(-dt / wind_tau);
		const float theta_relax = dt / (24000.0f + float(layer) * 9000.0f);
		const float q_relax = dt / (18000.0f + float(layer) * 7000.0f);
		const float cond_frac = std::min(dt / 120.0f, 1.0f);
		const float cloud_decay = std::exp(-dt / 10800.0f);

		for (int y = 0; y < h; ++y) {
			float lat = is_global
				? HALF_PI_F - PI_F * (float(y) + 0.5f) / float(h)
				: local_lat;
			float coslat = std::max(std::cos(lat), 0.12f);
			float dx = is_global ? std::max(TAU_F * PLANET_RADIUS_M * coslat / float(w), 8000.0f) : LOCAL_CELL_M;
			float dy = is_global ? PI_F * PLANET_RADIUS_M / float(h) : LOCAL_CELL_M;
			float coriolis = 2.0f * ROTATION_RATE * std::sin(lat);
			float alat = std::abs(lat);
			float sinlat = std::sin(alat);
			float surface_t = 302.0f - 48.0f * std::pow(sinlat, 1.35f);
			float clim_actual_t = std::clamp(surface_t - 0.00615f * height_m, 205.0f, 310.0f);
			float clim_theta = clim_actual_t / sf;
			float clim_q = std::clamp((0.0185f - 0.0125f * std::pow(sinlat, 1.2f))
				* std::exp(-height_m / 2450.0f) + 0.00008f, 0.00003f, 0.024f);
			float upper = float(layer) / float(LAYERS - 1);
			float target_u = (7.0f + 34.0f * upper)
				* std::exp(-std::pow((alat - 0.73f) / 0.27f, 2.0f));
			target_u += -9.0f * (1.0f - upper * 0.55f)
				* std::exp(-std::pow((alat - 0.30f) / 0.25f, 2.0f));

			for (int x = 0; x < w; x += 8) {
				for (int k = 0; k < 8; ++k) {
					int xx = x + k;
					int c = is_global ? global_cell(xx, y) : local_cell(xx, y);
					float cu = a.u[off + c];
					float cv = a.v[off + c];
					int ax = is_global
						? wrap_x(int(std::floor(float(xx) - cu * dt / dx)), w)
						: std::clamp(int(std::floor(float(xx) - cu * dt / dx)), 0, w - 1);
					int ay = is_global
						? clamp_y(int(std::floor(float(y) + cv * dt / dy)), h)
						: clamp_y(int(std::floor(float(y) - cv * dt / dy)), h);
					adv_idx[k] = ax + ay * w;
					east_idx[k] = (is_global ? wrap_x(xx + 1, w) : std::min(xx + 1, w - 1)) + y * w;
					west_idx[k] = (is_global ? wrap_x(xx - 1, w) : std::max(xx - 1, 0)) + y * w;
					int ny = is_global ? std::max(y - 1, 0) : std::min(y + 1, h - 1);
					int sy = is_global ? std::min(y + 1, h - 1) : std::max(y - 1, 0);
					north_idx[k] = xx + ny * w;
					south_idx[k] = xx + sy * w;
				}

				__m256i ai = _mm256_load_si256(reinterpret_cast<const __m256i *>(adv_idx));
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

				__m256 theta = _mm256_i32gather_ps(theta_base, ai, 4);
				__m256 q = _mm256_i32gather_ps(q_base, ai, 4);
				__m256 u = _mm256_i32gather_ps(u_base, ai, 4);
				__m256 v = _mm256_i32gather_ps(v_base, ai, 4);
				__m256 liquid = _mm256_i32gather_ps(liq_base, ai, 4);
				__m256 ice = _mm256_i32gather_ps(ice_base, ai, 4);
				__m256 pressure = _mm256_i32gather_ps(p_base, ai, 4);

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

				__m256 dpde = _mm256_mul_ps(_mm256_sub_ps(pe, pw), _mm256_set1_ps(0.5f / dx));
				__m256 dpdn = _mm256_mul_ps(_mm256_sub_ps(pn, ps), _mm256_set1_ps(0.5f / dy));
				__m256 div = _mm256_add_ps(
					_mm256_mul_ps(_mm256_sub_ps(ue, uw), _mm256_set1_ps(0.5f / dx)),
					_mm256_mul_ps(_mm256_sub_ps(vn, vs), _mm256_set1_ps(0.5f / dy)));
				__m256 vort = _mm256_sub_ps(
					_mm256_mul_ps(_mm256_sub_ps(ve, vw), _mm256_set1_ps(0.5f / dx)),
					_mm256_mul_ps(_mm256_sub_ps(un, us), _mm256_set1_ps(0.5f / dy)));

				_mm256_storeu_ps(&a.divergence[off + x + y * w], div);
				_mm256_storeu_ps(&a.vorticity[off + x + y * w], vort);

				// Horizontal momentum in local east/north coordinates.
				__m256 old_u = u;
				u = _mm256_fmadd_ps(
					_mm256_sub_ps(_mm256_mul_ps(v, _mm256_set1_ps(coriolis)), _mm256_div_ps(dpde, _mm256_set1_ps(rho))),
					_mm256_set1_ps(dt), u);
				v = _mm256_fmadd_ps(
					_mm256_sub_ps(_mm256_sub_ps(zero, _mm256_div_ps(dpdn, _mm256_set1_ps(rho))), _mm256_mul_ps(old_u, _mm256_set1_ps(coriolis))),
					_mm256_set1_ps(dt), v);
				u = _mm256_mul_ps(u, _mm256_set1_ps(wind_damp));
				v = _mm256_mul_ps(v, _mm256_set1_ps(wind_damp));
				u = _mm256_fmadd_ps(_mm256_sub_ps(_mm256_set1_ps(target_u), u), _mm256_set1_ps(dt / 43200.0f), u);

				// Thermodynamic relaxation supplies large-scale radiative/surface forcing.
				theta = _mm256_fmadd_ps(_mm256_sub_ps(_mm256_set1_ps(clim_theta), theta), _mm256_set1_ps(theta_relax), theta);
				q = _mm256_fmadd_ps(_mm256_sub_ps(_mm256_set1_ps(clim_q), q), _mm256_set1_ps(q_relax), q);

				// Pressure/geopotential anomalies react to thermal anomalies and horizontal
				// convergence. A small Laplacian damps grid-scale acoustic noise.
				__m256 thermal = _mm256_sub_ps(theta, _mm256_set1_ps(clim_theta));
				__m256 moisture = _mm256_sub_ps(q, _mm256_set1_ps(clim_q));
				__m256 target_p = _mm256_sub_ps(
					_mm256_mul_ps(thermal, _mm256_set1_ps(-70.0f * sigma)),
					_mm256_mul_ps(moisture, _mm256_set1_ps(12000.0f)));
				pressure = _mm256_fmadd_ps(_mm256_sub_ps(target_p, pressure), _mm256_set1_ps(dt / 18000.0f), pressure);
				pressure = _mm256_fnmadd_ps(div, _mm256_set1_ps(3000.0f * sigma * dt), pressure);
				__m256 pavg = _mm256_mul_ps(_mm256_add_ps(_mm256_add_ps(pe, pw), _mm256_add_ps(pn, ps)), _mm256_set1_ps(0.25f));
				pressure = _mm256_fmadd_ps(_mm256_sub_ps(pavg, pressure), _mm256_set1_ps(0.045f), pressure);

				// Moist microphysics. q is true specific humidity; cloud liquid/ice are
				// condensate mixing ratios. Latent heating feeds back into theta.
				__m256 temperature = _mm256_mul_ps(theta, _mm256_set1_ps(sf));
				__m256 pabs = clamp8(_mm256_add_ps(_mm256_set1_ps(P0 * sigma), pressure), 12000.0f, 105000.0f);
				__m256 qsat = qsat8(temperature, pabs);
				__m256 supersat = _mm256_max_ps(_mm256_sub_ps(q, qsat), zero);
				__m256 cond = _mm256_mul_ps(supersat, _mm256_set1_ps(cond_frac));
				q = _mm256_sub_ps(q, cond);
				__m256 ice_frac = clamp8(_mm256_mul_ps(_mm256_sub_ps(_mm256_set1_ps(268.0f), temperature), _mm256_set1_ps(1.0f / 20.0f)), 0.0f, 1.0f);
				liquid = _mm256_fmadd_ps(cond, _mm256_sub_ps(_mm256_set1_ps(1.0f), ice_frac), liquid);
				ice = _mm256_fmadd_ps(cond, ice_frac, ice);
				theta = _mm256_fmadd_ps(cond, _mm256_set1_ps(2500.0f / sf), theta);

				// Sub-saturated air re-evaporates cloud water, cooling the layer.
				__m256 deficit = _mm256_max_ps(_mm256_sub_ps(_mm256_mul_ps(qsat, _mm256_set1_ps(0.82f)), q), zero);
				__m256 cloud_total = _mm256_add_ps(liquid, ice);
				__m256 evap = _mm256_min_ps(cloud_total, _mm256_mul_ps(deficit, _mm256_set1_ps(std::min(dt / 420.0f, 0.35f))));
				__m256 liquid_share = _mm256_div_ps(liquid, _mm256_max_ps(cloud_total, _mm256_set1_ps(1e-7f)));
				liquid = _mm256_max_ps(_mm256_sub_ps(liquid, _mm256_mul_ps(evap, liquid_share)), zero);
				ice = _mm256_max_ps(_mm256_sub_ps(ice, _mm256_mul_ps(evap, _mm256_sub_ps(_mm256_set1_ps(1.0f), liquid_share))), zero);
				q = _mm256_add_ps(q, evap);
				theta = _mm256_fnmadd_ps(evap, _mm256_set1_ps(2100.0f / sf), theta);

				// Freeze/melt phase conversion.
				__m256 freeze_strength = clamp8(_mm256_mul_ps(_mm256_sub_ps(_mm256_set1_ps(260.0f), temperature), _mm256_set1_ps(1.0f / 14.0f)), 0.0f, 1.0f);
				__m256 freeze = _mm256_mul_ps(liquid, _mm256_mul_ps(freeze_strength, _mm256_set1_ps(std::min(dt / 900.0f, 0.25f))));
				liquid = _mm256_sub_ps(liquid, freeze);
				ice = _mm256_add_ps(ice, freeze);
				__m256 melt_strength = clamp8(_mm256_mul_ps(_mm256_sub_ps(temperature, _mm256_set1_ps(273.15f)), _mm256_set1_ps(1.0f / 8.0f)), 0.0f, 1.0f);
				__m256 melt = _mm256_mul_ps(ice, _mm256_mul_ps(melt_strength, _mm256_set1_ps(std::min(dt / 700.0f, 0.30f))));
				ice = _mm256_sub_ps(ice, melt);
				liquid = _mm256_add_ps(liquid, melt);

				// Autoconversion/fallout. Upper ice falls more slowly than warm rain.
				__m256 rain_excess = _mm256_max_ps(_mm256_sub_ps(liquid, _mm256_set1_ps(0.00115f)), zero);
				__m256 snow_excess = _mm256_max_ps(_mm256_sub_ps(ice, _mm256_set1_ps(0.00075f)), zero);
				__m256 rain_out = _mm256_mul_ps(rain_excess, _mm256_set1_ps(std::min(dt / 650.0f, 0.35f)));
				__m256 snow_out = _mm256_mul_ps(snow_excess, _mm256_set1_ps(std::min(dt / 1300.0f, 0.25f)));
				liquid = _mm256_sub_ps(liquid, rain_out);
				ice = _mm256_sub_ps(ice, snow_out);
				liquid = _mm256_mul_ps(liquid, _mm256_set1_ps(cloud_decay));
				ice = _mm256_mul_ps(ice, _mm256_set1_ps(cloud_decay));

				int base = x + y * w;
				__m256 pcol = _mm256_loadu_ps(&a.nprecip[base]);
				__m256 fallout = _mm256_add_ps(rain_out, _mm256_mul_ps(snow_out, _mm256_set1_ps(0.70f)));
				pcol = _mm256_add_ps(pcol, _mm256_mul_ps(fallout, _mm256_set1_ps(460.0f)));
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
}

void WeatherNative::vertical_pass(Atmosphere &a, bool is_global, float dt) {
	const __m256 zero = _mm256_setzero_ps();
	const __m256 one = _mm256_set1_ps(1.0f);
	const float max_mix = is_global ? 0.085f : 0.12f;

	for (int interface_index = 0; interface_index < INTERFACES; ++interface_index) {
		int lo = interface_index;
		int hi = interface_index + 1;
		int lo_off = a.layer_offset(lo);
		int hi_off = a.layer_offset(hi);
		int flux_off = a.interface_offset(interface_index);

		for (int c = 0; c < a.cells; c += 8) {
			__m256 th_lo = _mm256_loadu_ps(&a.ntheta[lo_off + c]);
			__m256 th_hi = _mm256_loadu_ps(&a.ntheta[hi_off + c]);
			__m256 q_lo = _mm256_loadu_ps(&a.nq[lo_off + c]);
			__m256 q_hi = _mm256_loadu_ps(&a.nq[hi_off + c]);
			__m256 div_lo = _mm256_loadu_ps(&a.divergence[lo_off + c]);
			__m256 precip = _mm256_loadu_ps(&a.nprecip[c]);

			// Positive theta decrease with height is statically unstable. Low-level
			// convergence and moisture then convert that instability into upward mass
			// transport. Heavy precipitation can generate a weaker downdraft branch.
			__m256 instability = clamp8(_mm256_mul_ps(_mm256_add_ps(_mm256_sub_ps(th_lo, th_hi), _mm256_set1_ps(1.5f)), _mm256_set1_ps(1.0f / 10.0f)), 0.0f, 1.0f);
			__m256 moisture = clamp8(_mm256_mul_ps(q_lo, _mm256_set1_ps(1.0f / 0.018f)), 0.0f, 1.0f);
			__m256 convergence_up = _mm256_mul_ps(_mm256_max_ps(_mm256_sub_ps(zero, div_lo), zero), _mm256_set1_ps(0.58f));
			__m256 convective_up = _mm256_mul_ps(_mm256_mul_ps(instability, moisture), _mm256_set1_ps(2.2e-4f));
			__m256 downdraft = _mm256_mul_ps(precip, _mm256_set1_ps(5.5e-5f));
			__m256 rate = clamp8(_mm256_sub_ps(_mm256_add_ps(convergence_up, convective_up), downdraft), -1.8e-4f, 6.0e-4f);
			_mm256_storeu_ps(&a.mass_flux[flux_off + c], rate);

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

	if (!is_global) {
		nudge_local_boundaries(dt);
	}
}

void WeatherNative::diagnose(Atmosphere &a, bool is_global) {
	const int w = a.width;
	const int h = a.height;
	const float local_lat = std::asin(std::clamp(local_center.y, -1.0f, 1.0f));
	alignas(32) int east_idx[8], west_idx[8], north_idx[8], south_idx[8];

	for (int layer = 0; layer < LAYERS; ++layer) {
		int off = a.layer_offset(layer);
		int below_layer = std::max(layer - 1, 0);
		int above_layer = std::min(layer + 1, LAYERS - 1);
		int below_off = a.layer_offset(below_layer);
		int above_off = a.layer_offset(above_layer);
		float dsigma = std::max(std::abs(SIGMA[above_layer] - SIGMA[below_layer]), 0.05f);

		for (int y = 0; y < h; ++y) {
			float lat = is_global ? HALF_PI_F - PI_F * (float(y) + 0.5f) / float(h) : local_lat;
			float coslat = std::max(std::cos(lat), 0.12f);
			float dx = is_global ? std::max(TAU_F * PLANET_RADIUS_M * coslat / float(w), 8000.0f) : LOCAL_CELL_M;
			float dy = is_global ? PI_F * PLANET_RADIUS_M / float(h) : LOCAL_CELL_M;
			float coriolis = 2.0f * ROTATION_RATE * std::sin(lat);

			for (int x = 0; x < w; x += 8) {
				for (int k = 0; k < 8; ++k) {
					int xx = x + k;
					east_idx[k] = (is_global ? wrap_x(xx + 1, w) : std::min(xx + 1, w - 1)) + y * w;
					west_idx[k] = (is_global ? wrap_x(xx - 1, w) : std::max(xx - 1, 0)) + y * w;
					int ny = is_global ? std::max(y - 1, 0) : std::min(y + 1, h - 1);
					int sy = is_global ? std::min(y + 1, h - 1) : std::max(y - 1, 0);
					north_idx[k] = xx + ny * w;
					south_idx[k] = xx + sy * w;
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
				__m256 div = _mm256_add_ps(
					_mm256_mul_ps(_mm256_sub_ps(ue, uw), _mm256_set1_ps(0.5f / dx)),
					_mm256_mul_ps(_mm256_sub_ps(vn, vs), _mm256_set1_ps(0.5f / dy)));
				__m256 vort = _mm256_sub_ps(
					_mm256_mul_ps(_mm256_sub_ps(ve, vw), _mm256_set1_ps(0.5f / dx)),
					_mm256_mul_ps(_mm256_sub_ps(un, us), _mm256_set1_ps(0.5f / dy)));
				int base = x + y * w;
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
	local_center = d.normalized();
	Vector3 pole = std::abs(local_center.y) > 0.92f ? Vector3(1, 0, 0) : Vector3(0, 1, 0);
	local_east = pole.cross(local_center).normalized();
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
	// six global sigma levels. A future remapping pass can replace this reset.
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

void WeatherNative::initialize_local() {
	float half = LOCAL_CELL_M * float(LOCAL_W) * 0.5f;
	for (int y = 0; y < LOCAL_H; ++y) {
		for (int x = 0; x < LOCAL_W; ++x) {
			float ex = (float(x) + 0.5f) * LOCAL_CELL_M - half;
			float ny = (float(y) + 0.5f) * LOCAL_CELL_M - half;
			Vector3 d = (local_center + local_east * (ex / PLANET_RADIUS_M)
				+ local_north * (ny / PLANET_RADIUS_M)).normalized();
			int c = x + y * LOCAL_W;
			for (int layer = 0; layer < LAYERS; ++layer) {
				float theta, q, u, v, liquid, ice, pressure;
				global_state_at_dir_layer(d, layer, theta, q, u, v, liquid, ice, pressure);
				int i = local_atm.layer_offset(layer) + c;
				local_atm.theta[i] = theta;
				local_atm.q[i] = q;
				local_atm.u[i] = u;
				local_atm.v[i] = v;
				local_atm.liquid[i] = liquid;
				local_atm.ice[i] = ice;
				local_atm.pressure[i] = pressure;
			}
			local_atm.precip[c] = 0.0f;
		}
	}
	local_initialized = true;
	diagnose(local_atm, false);
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
	horizontal_pass(local_atm, false, dt);
	vertical_pass(local_atm, false, dt);
	swap_state(local_atm);
	diagnose(local_atm, false);
}

static PackedFloat32Array weather_rgba_from(const WeatherNative::Atmosphere &a) {
	PackedFloat32Array out;
	out.resize(a.cells * 4);
	float *w = out.ptrw();
	for (int c = 0; c < a.cells; ++c) {
		float condensate = 0.0f;
		float max_ascent = 0.0f;
		float max_rotation = 0.0f;
		float max_shear = 0.0f;
		for (int layer = 0; layer < WeatherNative::LAYERS; ++layer) {
			int i = a.layer_offset(layer) + c;
			float layer_weight = 1.0f - 0.055f * float(layer);
			condensate += (a.liquid[i] + a.ice[i]) * layer_weight;
			if (layer <= 3) max_rotation = std::max(max_rotation, std::abs(a.vorticity[i]));
			max_shear = std::max(max_shear, a.shear[i]);
		}
		for (int interface_index = 0; interface_index < WeatherNative::INTERFACES; ++interface_index) {
			max_ascent = std::max(max_ascent, a.mass_flux[a.interface_offset(interface_index) + c]);
		}
		int l0 = a.layer_offset(0) + c;
		int l2 = a.layer_offset(2) + c;
		float instability = std::clamp((a.theta[l0] - a.theta[l2] + 1.5f) / 11.0f, 0.0f, 1.0f);
		float moisture = std::clamp(a.q[l0] / 0.018f, 0.0f, 1.0f);
		float ascent = std::clamp(max_ascent / 5.0e-4f, 0.0f, 1.0f);
		float rotation = std::clamp(max_rotation / 0.0016f, 0.0f, 1.0f);
		float shear = std::clamp(max_shear / 38.0f, 0.0f, 1.0f);
		float storm = std::clamp((0.36f * ascent + 0.24f * instability + 0.20f * rotation + 0.20f * shear) * (0.35f + 0.65f * moisture), 0.0f, 1.0f);
		float cloud = std::clamp(1.0f - std::exp(-condensate * 180.0f) + storm * 0.18f, 0.0f, 1.0f);
		w[c * 4 + 0] = cloud;
		w[c * 4 + 1] = storm;
		w[c * 4 + 2] = std::clamp(a.precip[c], 0.0f, 1.0f);
		w[c * 4 + 3] = std::clamp(0.5f + a.pressure[l0] / 12000.0f, 0.0f, 1.0f);
	}
	return out;
}

static PackedFloat32Array diagnostics_rgba_from(const WeatherNative::Atmosphere &a) {
	PackedFloat32Array out;
	out.resize(a.cells * 4);
	float *w = out.ptrw();
	for (int c = 0; c < a.cells; ++c) {
		// Select the strongest signed low/mid-level rotation so cyclonic and
		// anticyclonic structures remain visible instead of cancelling vertically.
		float vort = 0.0f;
		for (int layer = 0; layer <= 3; ++layer) {
			float z = a.vorticity[a.layer_offset(layer) + c];
			if (std::abs(z) > std::abs(vort)) vort = z;
		}
		float div = a.divergence[a.layer_offset(1) + c];
		float pv = a.potential_vorticity[a.layer_offset(4) + c];
		float shear = 0.0f;
		for (int layer = 0; layer < WeatherNative::LAYERS; ++layer) {
			shear = std::max(shear, a.shear[a.layer_offset(layer) + c]);
		}
		w[c * 4 + 0] = std::clamp(0.5f + vort / 0.008f, 0.0f, 1.0f);
		w[c * 4 + 1] = std::clamp(0.5f + div / 0.006f, 0.0f, 1.0f);
		w[c * 4 + 2] = std::clamp(0.5f + pv / 0.030f, 0.0f, 1.0f);
		w[c * 4 + 3] = std::clamp(shear / 55.0f, 0.0f, 1.0f);
	}
	return out;
}

PackedFloat32Array WeatherNative::get_global_weather_rgba() const {
	return weather_rgba_from(global_atm);
}

PackedFloat32Array WeatherNative::get_local_weather_rgba() const {
	return weather_rgba_from(local_atm);
}

PackedFloat32Array WeatherNative::get_global_diagnostics_rgba() const {
	return diagnostics_rgba_from(global_atm);
}

PackedFloat32Array WeatherNative::get_local_diagnostics_rgba() const {
	return diagnostics_rgba_from(local_atm);
}

} // namespace godot

#include "weather_native.h"

#include <godot_cpp/core/class_db.hpp>
#include <algorithm>
#include <cmath>
#include <immintrin.h>

namespace godot {

static inline __m256 clamp8(__m256 v, float lo, float hi) {
	return _mm256_min_ps(_mm256_set1_ps(hi), _mm256_max_ps(_mm256_set1_ps(lo), v));
}

static inline __m256 exp_approx8(__m256 x) {
	const __m256 one = _mm256_set1_ps(1.0f);
	__m256 x2 = _mm256_mul_ps(x, x);
	__m256 x3 = _mm256_mul_ps(x2, x);
	__m256 x4 = _mm256_mul_ps(x2, x2);
	__m256 x5 = _mm256_mul_ps(x4, x);
	return _mm256_add_ps(one,
		_mm256_add_ps(x,
		_mm256_add_ps(_mm256_mul_ps(x2, _mm256_set1_ps(0.5f)),
		_mm256_add_ps(_mm256_mul_ps(x3, _mm256_set1_ps(1.0f / 6.0f)),
		_mm256_add_ps(_mm256_mul_ps(x4, _mm256_set1_ps(1.0f / 24.0f)),
		_mm256_mul_ps(x5, _mm256_set1_ps(1.0f / 120.0f)))))));
}

void WeatherNative::Fields::resize(size_t n) {
	for (auto *a : {&temp, &pressure, &humidity, &u, &v, &cloud, &cape, &vort, &precip,
		&nt, &np, &nq, &nu, &nv, &nc, &ncap, &nvor, &nrain}) {
		a->assign(n, 0.0f);
	}
}

int WeatherNative::wrap_x(int x, int w) { x %= w; return x < 0 ? x + w : x; }
int WeatherNative::clamp_y(int y, int h) { return std::max(0, std::min(h - 1, y)); }
int WeatherNative::gi(int x, int y) { return wrap_x(x, GLOBAL_W) + clamp_y(y, GLOBAL_H) * GLOBAL_W; }
int WeatherNative::li(int x, int y) { return std::max(0, std::min(LOCAL_W - 1, x)) + std::max(0, std::min(LOCAL_H - 1, y)) * LOCAL_W; }

WeatherNative::WeatherNative() {
	g.resize(GLOBAL_W * GLOBAL_H);
	l.resize(LOCAL_W * LOCAL_H);
}

void WeatherNative::_bind_methods() {
	ClassDB::bind_method(D_METHOD("initialize", "seed"), &WeatherNative::initialize);
	ClassDB::bind_method(D_METHOD("step_global", "dt"), &WeatherNative::step_global);
	ClassDB::bind_method(D_METHOD("set_local_center", "center_dir"), &WeatherNative::set_local_center);
	ClassDB::bind_method(D_METHOD("step_local", "dt"), &WeatherNative::step_local);
	ClassDB::bind_method(D_METHOD("get_global_weather_rgba"), &WeatherNative::get_global_weather_rgba);
	ClassDB::bind_method(D_METHOD("get_local_weather_rgba"), &WeatherNative::get_local_weather_rgba);
	ClassDB::bind_method(D_METHOD("get_local_center"), &WeatherNative::get_local_center);
	ClassDB::bind_method(D_METHOD("get_local_east"), &WeatherNative::get_local_east);
	ClassDB::bind_method(D_METHOD("get_local_north"), &WeatherNative::get_local_north);
	ClassDB::bind_method(D_METHOD("get_local_span_m"), &WeatherNative::get_local_span_m);
}

void WeatherNative::initialize(int64_t p_seed) {
	seed = uint64_t(p_seed);
	initialize_global();
	local_initialized = false;
}

void WeatherNative::initialize_global() {
	const float phase = float(seed & 0xffffu) * 0.000173f;
	for (int y = 0; y < GLOBAL_H; ++y) {
		float lat = float(M_PI_2) - float(M_PI) * (float(y) + 0.5f) / float(GLOBAL_H);
		float alat = std::abs(lat);
		float jet = 31.0f * std::exp(-std::pow((alat - 0.733f) / 0.244f, 2.0f));
		float trade = -8.0f * std::exp(-std::pow((alat - 0.314f) / 0.262f, 2.0f));
		for (int x = 0; x < GLOBAL_W; ++x) {
			int i = x + y * GLOBAL_W;
			float lon = float(2.0 * M_PI) * (float(x) + 0.5f) / float(GLOBAL_W);
			float wave = 0.50f * std::sin(lon * 3.0f + lat * 1.7f + phase)
				+ 0.25f * std::sin(lon * 7.0f - lat * 2.3f);
			float syn = std::sin(lon * 2.0f + lat * 4.0f + phase * 3.1f);
			g.temp[i] = 300.0f - 50.0f * std::pow(std::sin(alat), 1.35f) + wave * 2.5f;
			g.pressure[i] = syn * 850.0f + wave * 350.0f;
			g.humidity[i] = std::clamp(0.70f - 0.34f * std::sin(alat) + 0.12f * wave, 0.12f, 0.96f);
			g.u[i] = jet + trade + 4.0f * wave;
			g.v[i] = 3.0f * std::cos(lon * 2.0f + lat * 3.0f);
			g.cloud[i] = std::clamp((g.humidity[i] - 0.62f) * 1.7f, 0.0f, 0.8f);
			g.cape[i] = std::max(0.0f, (g.temp[i] - 286.0f) * g.humidity[i] * 95.0f);
		}
	}
}

float WeatherNative::sample_global(const std::vector<float> &a, float x, float y) const {
	int x0 = int(std::floor(x));
	int y0 = int(std::floor(y));
	float fx = x - float(x0), fy = y - float(y0);
	float a00 = a[gi(x0, y0)], a10 = a[gi(x0 + 1, y0)];
	float a01 = a[gi(x0, y0 + 1)], a11 = a[gi(x0 + 1, y0 + 1)];
	return std::lerp(std::lerp(a00, a10, fx), std::lerp(a01, a11, fx), fy);
}

float WeatherNative::sample_local(const std::vector<float> &a, float x, float y) const {
	int x0 = int(std::floor(x));
	int y0 = int(std::floor(y));
	float fx = x - float(x0), fy = y - float(y0);
	float a00 = a[li(x0, y0)], a10 = a[li(x0 + 1, y0)];
	float a01 = a[li(x0, y0 + 1)], a11 = a[li(x0 + 1, y0 + 1)];
	return std::lerp(std::lerp(a00, a10, fx), std::lerp(a01, a11, fx), fy);
}

void WeatherNative::swap_global() {
	g.temp.swap(g.nt); g.pressure.swap(g.np); g.humidity.swap(g.nq); g.u.swap(g.nu); g.v.swap(g.nv);
	g.cloud.swap(g.nc); g.cape.swap(g.ncap); g.vort.swap(g.nvor); g.precip.swap(g.nrain);
}

void WeatherNative::swap_local() {
	l.temp.swap(l.nt); l.pressure.swap(l.np); l.humidity.swap(l.nq); l.u.swap(l.nu); l.v.swap(l.nv);
	l.cloud.swap(l.nc); l.cape.swap(l.ncap); l.vort.swap(l.nvor); l.precip.swap(l.nrain);
}

void WeatherNative::step_global(float dt) {
	const __m256 rho = _mm256_set1_ps(1.18f);
	const __m256 damp = _mm256_set1_ps(0.998f);
	const __m256 zero = _mm256_setzero_ps();
	alignas(32) int adv[8], east[8], west[8], north[8], south[8];

	for (int y = 0; y < GLOBAL_H; ++y) {
		float lat = float(M_PI_2) - float(M_PI) * (float(y) + 0.5f) / float(GLOBAL_H);
		float coslat = std::max(std::cos(lat), 0.12f);
		float mx = std::max(float(2.0 * M_PI) * PLANET_RADIUS_M * coslat / float(GLOBAL_W), 12000.0f);
		float my = float(M_PI) * PLANET_RADIUS_M / float(GLOBAL_H);
		float coriolis = 2.0f * 7.2921159e-5f * std::sin(lat);
		float base_t = 300.0f - 50.0f * std::pow(std::sin(std::abs(lat)), 1.35f);

		for (int x = 0; x < GLOBAL_W; x += 8) {
			for (int k = 0; k < 8; ++k) {
				int xx = x + k, i = gi(xx, y);
				int ax = wrap_x(int(std::floor(float(xx) - g.u[i] * dt / mx)), GLOBAL_W);
				int ay = clamp_y(int(std::floor(float(y) + g.v[i] * dt / my)), GLOBAL_H);
				adv[k] = gi(ax, ay); east[k] = gi(xx + 1, y); west[k] = gi(xx - 1, y);
				north[k] = gi(xx, y - 1); south[k] = gi(xx, y + 1);
			}
			__m256i ai = _mm256_load_si256((__m256i *)adv);
			__m256i ei = _mm256_load_si256((__m256i *)east), wi = _mm256_load_si256((__m256i *)west);
			__m256i ni = _mm256_load_si256((__m256i *)north), si = _mm256_load_si256((__m256i *)south);

			__m256 t = _mm256_i32gather_ps(g.temp.data(), ai, 4);
			__m256 p = _mm256_i32gather_ps(g.pressure.data(), ai, 4);
			__m256 q = _mm256_i32gather_ps(g.humidity.data(), ai, 4);
			__m256 u = _mm256_i32gather_ps(g.u.data(), ai, 4);
			__m256 v = _mm256_i32gather_ps(g.v.data(), ai, 4);
			__m256 c = _mm256_i32gather_ps(g.cloud.data(), ai, 4);
			__m256 cape = _mm256_i32gather_ps(g.cape.data(), ai, 4);

			__m256 pe = _mm256_i32gather_ps(g.pressure.data(), ei, 4), pw = _mm256_i32gather_ps(g.pressure.data(), wi, 4);
			__m256 pn = _mm256_i32gather_ps(g.pressure.data(), ni, 4), ps = _mm256_i32gather_ps(g.pressure.data(), si, 4);
			__m256 ue = _mm256_i32gather_ps(g.u.data(), ei, 4), uw = _mm256_i32gather_ps(g.u.data(), wi, 4);
			__m256 vn = _mm256_i32gather_ps(g.v.data(), ni, 4), vs = _mm256_i32gather_ps(g.v.data(), si, 4);

			__m256 dpdx = _mm256_mul_ps(_mm256_sub_ps(pe, pw), _mm256_set1_ps(0.5f / mx));
			__m256 dpdy = _mm256_mul_ps(_mm256_sub_ps(ps, pn), _mm256_set1_ps(0.5f / my));
			u = _mm256_fmadd_ps(_mm256_sub_ps(_mm256_mul_ps(v, _mm256_set1_ps(coriolis)), _mm256_div_ps(dpdx, rho)), _mm256_set1_ps(dt), u);
			v = _mm256_fmadd_ps(_mm256_sub_ps(zero, _mm256_add_ps(_mm256_div_ps(dpdy, rho), _mm256_mul_ps(u, _mm256_set1_ps(coriolis)))), _mm256_set1_ps(dt), v);

			__m256 div = _mm256_add_ps(
				_mm256_mul_ps(_mm256_sub_ps(ue, uw), _mm256_set1_ps(0.5f / mx)),
				_mm256_mul_ps(_mm256_sub_ps(vs, vn), _mm256_set1_ps(0.5f / my)));
			__m256 vort = _mm256_sub_ps(
				_mm256_mul_ps(_mm256_sub_ps(vs, vn), _mm256_set1_ps(0.5f / mx)),
				_mm256_mul_ps(_mm256_sub_ps(ue, uw), _mm256_set1_ps(0.5f / my)));

			__m256 eqp = _mm256_sub_ps(
				_mm256_mul_ps(_mm256_sub_ps(_mm256_set1_ps(base_t), t), _mm256_set1_ps(115.0f)),
				_mm256_mul_ps(_mm256_sub_ps(q, _mm256_set1_ps(0.55f)), _mm256_set1_ps(260.0f)));
			p = _mm256_fmadd_ps(_mm256_sub_ps(eqp, p), _mm256_set1_ps(dt / 18000.0f), p);
			// Tuned so a synoptic 1e-4 s^-1 convergence changes pressure by hundreds,
			// not millions, of pascals per model step.
			p = _mm256_fnmadd_ps(div, _mm256_set1_ps(8.5e4f * dt), p);

			__m256 sat = clamp8(_mm256_fmadd_ps(_mm256_sub_ps(t, _mm256_set1_ps(273.0f)), _mm256_set1_ps(0.0105f), _mm256_set1_ps(0.48f)), 0.30f, 0.96f);
			__m256 supersat = _mm256_max_ps(_mm256_sub_ps(q, sat), zero);
			__m256 lift = clamp8(_mm256_mul_ps(_mm256_sub_ps(zero, div), _mm256_set1_ps(90000.0f)), 0.0f, 1.0f);
			__m256 trigger = _mm256_mul_ps(clamp8(_mm256_mul_ps(_mm256_sub_ps(cape, _mm256_set1_ps(450.0f)), _mm256_set1_ps(1.0f / 1800.0f)), 0.0f, 1.0f), lift);
			__m256 cond = _mm256_mul_ps(
				_mm256_add_ps(_mm256_mul_ps(supersat, _mm256_set1_ps(0.15f)), _mm256_mul_ps(_mm256_mul_ps(trigger, q), _mm256_set1_ps(0.012f))),
				_mm256_set1_ps(dt / 60.0f));
			cond = _mm256_min_ps(cond, _mm256_mul_ps(q, _mm256_set1_ps(0.18f)));
			q = _mm256_sub_ps(q, cond);
			c = _mm256_fmadd_ps(cond, _mm256_set1_ps(2.6f), c);
			t = _mm256_fmadd_ps(cond, _mm256_set1_ps(7.47f), t);
			cape = _mm256_fmadd_ps(_mm256_mul_ps(_mm256_max_ps(_mm256_sub_ps(t, _mm256_set1_ps(284.0f)), zero), q), _mm256_set1_ps(0.035f * dt), cape);
			cape = _mm256_fnmadd_ps(trigger, _mm256_set1_ps(9.0f * dt), cape);
			c = _mm256_mul_ps(c, exp_approx8(_mm256_set1_ps(-dt / 7200.0f)));
			__m256 rain = clamp8(_mm256_add_ps(_mm256_mul_ps(_mm256_sub_ps(c, _mm256_set1_ps(0.16f)), _mm256_set1_ps(2.4f)), _mm256_mul_ps(trigger, _mm256_set1_ps(0.8f))), 0.0f, 1.0f);
			c = _mm256_max_ps(_mm256_fnmadd_ps(rain, _mm256_set1_ps(dt / 9000.0f), c), zero);

			int base = x + y * GLOBAL_W;
			_mm256_storeu_ps(&g.nt[base], clamp8(t, 220.0f, 318.0f));
			_mm256_storeu_ps(&g.np[base], clamp8(p, -5500.0f, 5500.0f));
			_mm256_storeu_ps(&g.nq[base], clamp8(q, 0.02f, 1.2f));
			_mm256_storeu_ps(&g.nu[base], clamp8(_mm256_mul_ps(u, damp), -85.0f, 85.0f));
			_mm256_storeu_ps(&g.nv[base], clamp8(_mm256_mul_ps(v, damp), -70.0f, 70.0f));
			_mm256_storeu_ps(&g.nc[base], clamp8(c, 0.0f, 1.4f));
			_mm256_storeu_ps(&g.ncap[base], clamp8(cape, 0.0f, 4200.0f));
			_mm256_storeu_ps(&g.nvor[base], clamp8(vort, -0.01f, 0.01f));
			_mm256_storeu_ps(&g.nrain[base], rain);
		}
	}
	swap_global();
}

void WeatherNative::update_local_basis(const Vector3 &d) {
	local_center = d.normalized();
	Vector3 pole = std::abs(local_center.y) > 0.92 ? Vector3(1, 0, 0) : Vector3(0, 1, 0);
	local_east = pole.cross(local_center).normalized();
	local_north = local_center.cross(local_east).normalized();
}

void WeatherNative::set_local_center(const Vector3 &d) {
	Vector3 wanted = d.normalized();
	if (!local_initialized) {
		update_local_basis(wanted);
		initialize_local();
		return;
	}
	// Recenter in chunks (~35 km) so the local weather does not move with every
	// player metre. The nest remains comfortably larger than the recenter threshold.
	if (local_center.dot(wanted) < 0.99939f) {
		update_local_basis(wanted);
		initialize_local();
	}
}

void WeatherNative::global_state_at_dir(const Vector3 &d, float &cloud, float &cape, float &rain,
	float &pressure, float &temp, float &humidity, float &u, float &v) const {
	Vector3 n = d.normalized();
	float lon = std::atan2(n.z, n.x); if (lon < 0.0f) lon += float(2.0 * M_PI);
	float lat = std::asin(std::clamp(float(n.y), -1.0f, 1.0f));
	float x = lon / float(2.0 * M_PI) * GLOBAL_W - 0.5f;
	float y = (float(M_PI_2) - lat) / float(M_PI) * GLOBAL_H - 0.5f;
	cloud = sample_global(g.cloud, x, y); cape = sample_global(g.cape, x, y); rain = sample_global(g.precip, x, y);
	pressure = sample_global(g.pressure, x, y); temp = sample_global(g.temp, x, y); humidity = sample_global(g.humidity, x, y);
	u = sample_global(g.u, x, y); v = sample_global(g.v, x, y);
}

void WeatherNative::initialize_local() {
	float half = LOCAL_CELL_M * LOCAL_W * 0.5f;
	for (int y = 0; y < LOCAL_H; ++y) {
		for (int x = 0; x < LOCAL_W; ++x) {
			float ex = (x + 0.5f) * LOCAL_CELL_M - half;
			float ny = (y + 0.5f) * LOCAL_CELL_M - half;
			Vector3 d = (local_center + local_east * (ex / PLANET_RADIUS_M) + local_north * (ny / PLANET_RADIUS_M)).normalized();
			float c, ca, r, p, t, q, u, v;
			global_state_at_dir(d, c, ca, r, p, t, q, u, v);
			int i = x + y * LOCAL_W;
			l.cloud[i] = c; l.cape[i] = ca; l.precip[i] = r; l.pressure[i] = p;
			l.temp[i] = t; l.humidity[i] = q; l.u[i] = u; l.v[i] = v;
		}
	}
	local_initialized = true;
}

void WeatherNative::step_local(float dt) {
	if (!local_initialized) initialize_local();
	const float cell = LOCAL_CELL_M;
	alignas(32) int adv[8], east[8], west[8], north[8], south[8];
	float half = LOCAL_CELL_M * LOCAL_W * 0.5f;

	for (int y = 0; y < LOCAL_H; ++y) {
		for (int x = 0; x < LOCAL_W; x += 8) {
			for (int k = 0; k < 8; ++k) {
				int xx = x + k, i = li(xx, y);
				int ax = int(std::floor(float(xx) - l.u[i] * dt / cell));
				int ay = int(std::floor(float(y) + l.v[i] * dt / cell));
				adv[k] = li(ax, ay); east[k] = li(xx + 1, y); west[k] = li(xx - 1, y);
				north[k] = li(xx, y - 1); south[k] = li(xx, y + 1);
			}
			__m256i ai = _mm256_load_si256((__m256i *)adv);
			__m256i ei = _mm256_load_si256((__m256i *)east), wi = _mm256_load_si256((__m256i *)west);
			__m256i ni = _mm256_load_si256((__m256i *)north), si = _mm256_load_si256((__m256i *)south);

			__m256 t = _mm256_i32gather_ps(l.temp.data(), ai, 4);
			__m256 p = _mm256_i32gather_ps(l.pressure.data(), ai, 4);
			__m256 q = _mm256_i32gather_ps(l.humidity.data(), ai, 4);
			__m256 u = _mm256_i32gather_ps(l.u.data(), ai, 4);
			__m256 v = _mm256_i32gather_ps(l.v.data(), ai, 4);
			__m256 c = _mm256_i32gather_ps(l.cloud.data(), ai, 4);
			__m256 cape = _mm256_i32gather_ps(l.cape.data(), ai, 4);

			__m256 dpdx = _mm256_mul_ps(_mm256_sub_ps(_mm256_i32gather_ps(l.pressure.data(), ei, 4), _mm256_i32gather_ps(l.pressure.data(), wi, 4)), _mm256_set1_ps(0.5f / cell));
			__m256 dpdy = _mm256_mul_ps(_mm256_sub_ps(_mm256_i32gather_ps(l.pressure.data(), si, 4), _mm256_i32gather_ps(l.pressure.data(), ni, 4)), _mm256_set1_ps(0.5f / cell));
			u = _mm256_fnmadd_ps(dpdx, _mm256_set1_ps(dt / 1.18f), u);
			v = _mm256_fnmadd_ps(dpdy, _mm256_set1_ps(dt / 1.18f), v);

			__m256 div = _mm256_mul_ps(_mm256_add_ps(
				_mm256_sub_ps(_mm256_i32gather_ps(l.u.data(), ei, 4), _mm256_i32gather_ps(l.u.data(), wi, 4)),
				_mm256_sub_ps(_mm256_i32gather_ps(l.v.data(), si, 4), _mm256_i32gather_ps(l.v.data(), ni, 4))), _mm256_set1_ps(0.5f / cell));
			__m256 vort = _mm256_mul_ps(_mm256_sub_ps(
				_mm256_sub_ps(_mm256_i32gather_ps(l.v.data(), ei, 4), _mm256_i32gather_ps(l.v.data(), wi, 4)),
				_mm256_sub_ps(_mm256_i32gather_ps(l.u.data(), si, 4), _mm256_i32gather_ps(l.u.data(), ni, 4))), _mm256_set1_ps(0.5f / cell));

			p = _mm256_fnmadd_ps(div, _mm256_set1_ps(4.0e4f * dt), p);
			__m256 lift = clamp8(_mm256_mul_ps(_mm256_sub_ps(_mm256_setzero_ps(), div), _mm256_set1_ps(30000.0f)), 0.0f, 1.0f);
			__m256 trigger = _mm256_mul_ps(clamp8(_mm256_mul_ps(_mm256_sub_ps(cape, _mm256_set1_ps(350.0f)), _mm256_set1_ps(1.0f / 1300.0f)), 0.0f, 1.0f), lift);
			__m256 sat = clamp8(_mm256_fmadd_ps(_mm256_sub_ps(t, _mm256_set1_ps(273.0f)), _mm256_set1_ps(0.0108f), _mm256_set1_ps(0.47f)), 0.30f, 0.97f);
			__m256 cond = _mm256_mul_ps(_mm256_add_ps(
				_mm256_mul_ps(_mm256_max_ps(_mm256_sub_ps(q, sat), _mm256_setzero_ps()), _mm256_set1_ps(0.22f)),
				_mm256_mul_ps(_mm256_mul_ps(trigger, q), _mm256_set1_ps(0.020f))), _mm256_set1_ps(dt / 20.0f));
			cond = _mm256_min_ps(cond, _mm256_mul_ps(q, _mm256_set1_ps(0.25f)));
			q = _mm256_sub_ps(q, cond);
			c = _mm256_fmadd_ps(cond, _mm256_set1_ps(3.4f), c);
			cape = _mm256_fnmadd_ps(trigger, _mm256_set1_ps(13.0f * dt), cape);
			__m256 rain = clamp8(_mm256_add_ps(_mm256_mul_ps(_mm256_sub_ps(c, _mm256_set1_ps(0.12f)), _mm256_set1_ps(2.8f)), trigger), 0.0f, 1.0f);

			alignas(32) float gt[8], gp[8], gq[8], gu[8], gv[8], gc[8], gca[8];
			for (int k = 0; k < 8; ++k) {
				float ex = (x + k + 0.5f) * LOCAL_CELL_M - half;
				float ny = (y + 0.5f) * LOCAL_CELL_M - half;
				Vector3 d = (local_center + local_east * (ex / PLANET_RADIUS_M) + local_north * (ny / PLANET_RADIUS_M)).normalized();
				float rr;
				global_state_at_dir(d, gc[k], gca[k], rr, gp[k], gt[k], gq[k], gu[k], gv[k]);
			}
			const __m256 nudge = _mm256_set1_ps(0.02f);
			t = _mm256_fmadd_ps(_mm256_sub_ps(_mm256_load_ps(gt), t), nudge, t);
			p = _mm256_fmadd_ps(_mm256_sub_ps(_mm256_load_ps(gp), p), nudge, p);
			q = _mm256_fmadd_ps(_mm256_sub_ps(_mm256_load_ps(gq), q), nudge, q);
			u = _mm256_fmadd_ps(_mm256_sub_ps(_mm256_load_ps(gu), u), nudge, u);
			v = _mm256_fmadd_ps(_mm256_sub_ps(_mm256_load_ps(gv), v), nudge, v);
			c = _mm256_fmadd_ps(_mm256_sub_ps(_mm256_load_ps(gc), c), _mm256_set1_ps(0.012f), c);
			cape = _mm256_fmadd_ps(_mm256_sub_ps(_mm256_load_ps(gca), cape), _mm256_set1_ps(0.01f), cape);

			int base = x + y * LOCAL_W;
			_mm256_storeu_ps(&l.nt[base], clamp8(t, 220.0f, 320.0f));
			_mm256_storeu_ps(&l.np[base], clamp8(p, -6000.0f, 6000.0f));
			_mm256_storeu_ps(&l.nq[base], clamp8(q, 0.02f, 1.2f));
			_mm256_storeu_ps(&l.nu[base], clamp8(u, -95.0f, 95.0f));
			_mm256_storeu_ps(&l.nv[base], clamp8(v, -85.0f, 85.0f));
			_mm256_storeu_ps(&l.nc[base], clamp8(c, 0.0f, 1.6f));
			_mm256_storeu_ps(&l.ncap[base], clamp8(cape, 0.0f, 5000.0f));
			_mm256_storeu_ps(&l.nvor[base], clamp8(vort, -0.03f, 0.03f));
			_mm256_storeu_ps(&l.nrain[base], rain);
		}
	}
	swap_local();
}

PackedFloat32Array WeatherNative::get_global_weather_rgba() const {
	PackedFloat32Array out;
	out.resize(GLOBAL_W * GLOBAL_H * 4);
	float *w = out.ptrw();
	for (int i = 0; i < GLOBAL_W * GLOBAL_H; ++i) {
		float storm = std::clamp(g.cape[i] / 3200.0f + std::abs(g.vort[i]) * 90.0f, 0.0f, 1.0f);
		w[i * 4 + 0] = std::clamp(0.12f + g.cloud[i] * 0.72f + g.humidity[i] * 0.16f, 0.0f, 1.0f);
		w[i * 4 + 1] = storm;
		w[i * 4 + 2] = std::clamp(g.precip[i], 0.0f, 1.0f);
		w[i * 4 + 3] = std::clamp(0.5f + g.pressure[i] / 11000.0f, 0.0f, 1.0f);
	}
	return out;
}

PackedFloat32Array WeatherNative::get_local_weather_rgba() const {
	PackedFloat32Array out;
	out.resize(LOCAL_W * LOCAL_H * 4);
	float *w = out.ptrw();
	for (int i = 0; i < LOCAL_W * LOCAL_H; ++i) {
		float storm = std::clamp(l.cape[i] / 3600.0f + std::abs(l.vort[i]) * 34.0f, 0.0f, 1.0f);
		w[i * 4 + 0] = std::clamp(0.10f + l.cloud[i] * 0.76f + l.humidity[i] * 0.14f, 0.0f, 1.0f);
		w[i * 4 + 1] = storm;
		w[i * 4 + 2] = std::clamp(l.precip[i], 0.0f, 1.0f);
		w[i * 4 + 3] = std::clamp(0.5f + l.pressure[i] / 12000.0f, 0.0f, 1.0f);
	}
	return out;
}

} // namespace godot

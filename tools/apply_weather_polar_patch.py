from pathlib import Path


def replace_exact(text, old, new, expected, label):
    count = text.count(old)
    if count != expected:
        raise SystemExit(f"{label}: expected {expected}, found {count}")
    return text.replace(old, new)


# Native core
p = Path("native/weather/src/weather_native.cpp")
s = p.read_text(encoding="utf-8")

s = replace_exact(
    s,
    "global_atm.v[i] = (2.0f + upper * 2.0f) * std::cos(lon * 2.0f + lat * 3.0f + upper * 1.85f);",
    "global_atm.v[i] = polar_taper * (2.0f + upper * 2.0f)\n\t\t\t\t\t* std::cos(lon * 2.0f + lat * 3.0f + upper * 1.85f);",
    1, "initial polar-v taper")

s = replace_exact(
    s,
    "global_atm.u[i] = jet + trade + wave * (3.0f + upper * 3.5f);",
    "// Separate circumpolar jet, strongest aloft near 59 degrees.\n"
    "\t\t\t\tfloat polar_jet = (3.0f + 30.0f * upper)\n"
    "\t\t\t\t\t* std::exp(-std::pow((alat - 1.03f) / 0.20f, 2.0f));\n"
    "\t\t\t\tglobal_atm.u[i] = jet + polar_jet + trade + wave * (3.0f + upper * 3.5f);",
    1, "initial polar jet")

s = replace_exact(
    s,
    "float dx = is_global ? TAU_F * PLANET_RADIUS_M * coslat / float(w) : LOCAL_CELL_M;",
    "// Reduced-grid metric in the polar cap: the 1024 longitude values\n"
    "\t\t\t// are not independent as circumference tends to zero.\n"
    "\t\t\tfloat metric_coslat = is_global ? std::max(std::abs(coslat), 0.10f) : 1.0f;\n"
    "\t\t\tfloat dx = is_global ? TAU_F * PLANET_RADIUS_M * metric_coslat / float(w) : LOCAL_CELL_M;",
    2, "polar dx metric")

s = replace_exact(
    s,
    "float curvature = is_global ? std::tan(lat) / PLANET_RADIUS_M : 0.0f;",
    "float metric_lat = is_global ? std::clamp(lat, -1.3962634f, 1.3962634f) : lat;\n"
    "\t\t\tfloat curvature = is_global ? std::tan(metric_lat) / PLANET_RADIUS_M : 0.0f;",
    2, "polar curvature cap")

hp = s.index("void WeatherNative::horizontal_pass(Atmosphere &a, bool is_global, float dt)")
prefix, h = s[:hp], s[hp:]
h = replace_exact(
    h,
    "float sinlat = std::sin(alat);\n\t\t\tfloat surface_t =",
    "float sinlat = std::sin(alat);\n"
    "\t\t\tfloat polar_forcing_taper = smoothstep01((HALF_PI_F - alat) / (PI_F / 12.0f));\n"
    "\t\t\tfloat surface_t =",
    1, "runtime polar forcing taper")
h = replace_exact(
    h,
    "target_u += -9.0f * (1.0f - upper * 0.55f)",
    "target_u += (3.0f + 30.0f * upper)\n"
    "\t\t\t\t* std::exp(-std::pow((alat - 1.03f) / 0.20f, 2.0f));\n"
    "\t\t\ttarget_u += -9.0f * (1.0f - upper * 0.55f)",
    1, "runtime polar jet")
h = replace_exact(
    h,
    "target_v_lane[k] = amplitude * 0.78f * std::sin(phase3)\n"
    "\t\t\t\t\t\t+ amplitude * (0.26f * std::cos(phase5) + 0.14f * std::sin(phase7));",
    "target_v_lane[k] = polar_forcing_taper * (\n"
    "\t\t\t\t\t\tamplitude * 0.78f * std::sin(phase3)\n"
    "\t\t\t\t\t\t+ amplitude * (0.26f * std::cos(phase5) + 0.14f * std::sin(phase7)));",
    1, "runtime polar-v taper")
s = prefix + h

cap_marker = "\tmeridional_filter(a.mass_flux, 0.36f, false);\n}\n\nvoid WeatherNative::center_global_pressure"
cap_code = """\tmeridional_filter(a.mass_flux, 0.36f, false);

\t// Collapse only the innermost ~1.4 degrees toward zonal symmetry. The
\t// Antarctic-like 50-70 degree annular jet stays outside this numerical cap.
\tauto zonal_relax = [&](std::vector<float> &field, int offset, int y, float strength) {
\t\tint row = offset + y * a.width;
\t\tdouble sum = 0.0;
\t\tfor (int x = 0; x < a.width; ++x) sum += field[row + x];
\t\tfloat mean = float(sum / double(a.width));
\t\tfor (int x = 0; x < a.width; ++x) field[row + x] = std::lerp(field[row + x], mean, strength);
\t};
\tstatic constexpr float CAP_STRENGTH[4] = {1.0f, 0.72f, 0.45f, 0.22f};
\tfor (int edge = 0; edge < 4; ++edge) {
\t\tfor (int y : {edge, a.height - 1 - edge}) {
\t\t\tfloat strength = CAP_STRENGTH[edge];
\t\t\tfor (int layer = 0; layer < LAYERS; ++layer) {
\t\t\t\tint off = a.layer_offset(layer);
\t\t\t\tzonal_relax(a.ntheta, off, y, strength);
\t\t\t\tzonal_relax(a.nq, off, y, strength);
\t\t\t\tzonal_relax(a.nu, off, y, strength);
\t\t\t\tzonal_relax(a.nliquid, off, y, strength);
\t\t\t\tzonal_relax(a.nice, off, y, strength);
\t\t\t\tzonal_relax(a.npressure, off, y, strength);
\t\t\t\tint row = off + y * a.width;
\t\t\t\tfor (int x = 0; x < a.width; ++x) a.nv[row + x] *= (1.0f - strength);
\t\t\t}
\t\t\tzonal_relax(a.nprecip, 0, y, strength);
\t\t\tfor (int interface_index = 0; interface_index < INTERFACES; ++interface_index) {
\t\t\t\tzonal_relax(a.mass_flux, a.interface_offset(interface_index), y, strength);
\t\t\t}
\t\t}
\t}
}

void WeatherNative::center_global_pressure"""
s = replace_exact(s, cap_marker, cap_code, 1, "polar core collapse")
p.write_text(s, encoding="utf-8")


# Sparse severe wrapper
p = Path("native/weather/src/weather_native_oklahoma.cpp")
s = p.read_text(encoding="utf-8")
s = replace_exact(
    s,
    "tuning_weights[HUMIDITY] = requested_humidity_weight * (is_global ? 0.12f : 0.0f);",
    "// Do not refill vapour from climatology once surface evaporation/dew is live.\n"
    "\ttuning_weights[HUMIDITY] = requested_humidity_weight\n"
    "\t\t* ((is_global && !surface_fields_ready) ? 0.12f : 0.0f);",
    1, "humidity water source")

helper_marker = "static inline int neighbour_cell(const WeatherNative::Atmosphere &a, bool is_global,"
helper = """// Remove the pure red/blue 2-delta-x pressure mode after all physical
// tendencies. A binomial Shapiro stencil leaves synoptic wavelengths almost
// untouched while damping a checkerboard with a dt-independent 4-hour e-fold.
static void suppress_global_pressure_checkerboard(WeatherNative::Atmosphere &a, float dt) {
\tstd::vector<float> source(a.npressure.begin(), a.npressure.end());
\tconst float strength = 1.0f - std::exp(-dt / (4.0f * 3600.0f));
\t#pragma omp parallel for schedule(static)
\tfor (int layer = 0; layer < WeatherNative::LAYERS; ++layer) {
\t\tint off = a.layer_offset(layer);
\t\tfor (int y = 0; y < a.height; ++y) {
\t\t\tint yn = std::max(y - 1, 0);
\t\t\tint ys = std::min(y + 1, a.height - 1);
\t\t\tfor (int x = 0; x < a.width; ++x) {
\t\t\t\tint xe = (x + 1) % a.width;
\t\t\t\tint xw = (x + a.width - 1) % a.width;
\t\t\t\tauto at = [&](int xx, int yy) -> float { return source[off + xx + yy * a.width]; };
\t\t\t\tint c = off + x + y * a.width;
\t\t\t\tfloat smooth = (4.0f * source[c]
\t\t\t\t\t+ 2.0f * (at(xe, y) + at(xw, y) + at(x, yn) + at(x, ys))
\t\t\t\t\t+ at(xe, yn) + at(xw, yn) + at(xe, ys) + at(xw, ys)) * (1.0f / 16.0f);
\t\t\t\ta.npressure[c] = std::lerp(source[c], smooth, strength);
\t\t\t}
\t\t}
\t}
}

static inline int neighbour_cell(const WeatherNative::Atmosphere &a, bool is_global,"""
s = replace_exact(s, helper_marker, helper, 1, "pressure Shapiro helper")
s = replace_exact(
    s,
    "vertical_pass(global_atm, true, dt);\n\tfilter_global_poles(global_atm);",
    "vertical_pass(global_atm, true, dt);\n\tsuppress_global_pressure_checkerboard(global_atm, dt);\n\tfilter_global_poles(global_atm);",
    1, "pressure Shapiro call")
p.write_text(s, encoding="utf-8")


# Weather map warp UI
p = Path("scripts/ui/weather_map.gd")
s = p.read_text(encoding="utf-8")
s = replace_exact(
    s, "const FIELD_BUILD_BUDGET_USEC := 2600",
    "const FIELD_BUILD_BUDGET_USEC := 2600\n"
    "const WARP_SPEEDS: Array[float] = [\n"
    "\t0.0, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0,\n"
    "\t64.0, 128.0, 256.0, 512.0, 1024.0, 2048.0, 4096.0, 8192.0,\n]",
    1, "warp speeds")
s = replace_exact(s, "var _warp_backlogged := false", "var _warp_backlogged := false\nvar _speed_slider_dragging := false", 1, "warp drag state")
s = replace_exact(s, "_speed_slider.max_value = 14.0", "_speed_slider.max_value = float(WARP_SPEEDS.size() - 1)", 1, "warp max")
s = replace_exact(
    s,
    '_speed_slider.tooltip_text = "Logarithmic weather warp: paused, then powers of two from 1× to 8192×. Above 256× the global model spins up and the local nest resynchronises afterward."',
    '_speed_slider.tooltip_text = "Discrete weather warp: pause, 0.25×, 0.5×, 1×, then powers of two to 8192×. Dragging previews and commits on release."',
    1, "warp tooltip")
s = replace_exact(
    s,
    "_speed_slider.value_changed.connect(_on_speed_slider_changed)",
    "_speed_slider.value_changed.connect(_on_speed_slider_changed)\n"
    "\t_speed_slider.drag_started.connect(_on_speed_slider_drag_started)\n"
    "\t_speed_slider.drag_ended.connect(_on_speed_slider_drag_ended)",
    1, "warp drag signals")

start = s.index("func _on_speed_slider_changed(slider_value: float) -> void:")
end = s.index("func _update_speed_label(value: float) -> void:", start)
funcs = """func _on_speed_slider_drag_started() -> void:
\t_speed_slider_dragging = true


func _on_speed_slider_drag_ended(value_changed: bool) -> void:
\t_speed_slider_dragging = false
\tif value_changed and _speed_slider != null:
\t\tWeatherSystem.set_simulation_speed(_slider_to_speed(_speed_slider.value))
\t_update_speed_label(WeatherSystem.simulation_speed)


func _on_speed_slider_changed(slider_value: float) -> void:
\tvar preview := _slider_to_speed(slider_value)
\tif not _speed_slider_dragging:
\t\tWeatherSystem.set_simulation_speed(preview)
\t_update_speed_label(preview)


func _on_external_speed_changed(value: float) -> void:
\tif _speed_slider != null and not _speed_slider_dragging:
\t\t_speed_slider.set_value_no_signal(_speed_to_slider(value))
\t_update_speed_label(value)


func _step_warp(steps: int) -> void:
\tvar slider_value := clampf(
\t\t_speed_to_slider(WeatherSystem.simulation_speed) + float(steps),
\t\t0.0, float(WARP_SPEEDS.size() - 1))
\tWeatherSystem.set_simulation_speed(_slider_to_speed(slider_value))


func _slider_to_speed(slider_value: float) -> float:
\tvar index := clampi(int(round(slider_value)), 0, WARP_SPEEDS.size() - 1)
\treturn WARP_SPEEDS[index]


func _speed_to_slider(speed: float) -> float:
\tif speed <= 0.01:
\t\treturn 0.0
\tvar best_index := 1
\tvar best_error := 1.0e30
\tfor i in range(1, WARP_SPEEDS.size()):
\t\tvar error := absf(log(maxf(speed, 0.001)) - log(WARP_SPEEDS[i]))
\t\tif error < best_error:
\t\t\tbest_error = error
\t\t\tbest_index = i
\treturn float(best_index)


"""
s = s[:start] + funcs + s[end:]
p.write_text(s, encoding="utf-8")


# Time-based wind streaks
p = Path("shaders/weather_wind_particles.gdshader")
s = p.read_text(encoding="utf-8")
start = s.index("// Streak travel per real second")
end = s.index("uniform float u_opacity", start)
uniforms = """// Advection window is model time. Trail time is physical: streak length is
// speed * trail_time / planet_radius, so fast jet-stream particles stay visible
// longer and draw proportionally longer paths.
uniform float u_advection_window_s = 36000.0;
uniform float u_trail_time_s = 1200.0;
uniform float u_animation_rate = 0.14;
uniform float u_min_line_length = 0.0022;
uniform float u_max_line_length = 0.036;
uniform float u_line_width = 0.00125;
uniform float u_speed_reference = 65.0;
"""
s = s[:start] + uniforms + s[end:]
s = replace_exact(s, "max(abs(cos(lat)), 0.075)", "max(abs(cos(lat)), 0.10)", 1, "wind polar metric")
s = replace_exact(
    s,
    "float life = fract(TIME * u_animation_rate + phase);\n\tfloat travel = life * u_particle_lifetime_s;",
    "vec4 seed_field = textureLod(u_wind, seed_uv, 0.0);\n"
    "\tfloat seed_speed = length(seed_field.rg);\n"
    "\tfloat speed01 = clamp(seed_speed / u_speed_reference, 0.0, 1.0);\n"
    "\tfloat persistence = mix(0.72, 1.75, speed01);\n"
    "\tfloat life = fract(TIME * u_animation_rate / persistence + phase);\n"
    "\tfloat travel = life * u_advection_window_s;",
    1, "wind persistence")
s = replace_exact(
    s,
    "float speed_scale = mix(0.52, 1.55, clamp(speed / u_speed_reference, 0.0, 1.0));\n"
    "\tfloat along = VERTEX.x * u_line_length * speed_scale * size_jitter;",
    "float trail_length = clamp(speed * u_trail_time_s / u_planet_radius_m,\n"
    "\t\tu_min_line_length, u_max_line_length);\n"
    "\tfloat along = VERTEX.x * trail_length * size_jitter;",
    1, "wind time trail")
p.write_text(s, encoding="utf-8")

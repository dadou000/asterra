shader_type spatial;
render_mode cull_back, diffuse_burley, specular_schlick_ggx, ambient_light_disabled, shadows_disabled;
/// Far-LOD celestial body surface for the multi-body schematic / space view.
///
/// Renders a body that is NOT the resident detailed terrain as a lit sphere:
/// its own terminator (day / night from `u_sun_dir`), an ambient floor, an
/// optional atmosphere rim, and -- when a per-body orbit-relief elevation
/// texture array is supplied -- real relief shading and a relief-perturbed
/// normal so a mountain range reads as a range from orbit or from an adjacent
/// body's surface. With no relief texture it degrades to a smooth lit sphere,
/// which still looks like a world instead of the old flat unshaded disc.
///
/// Lighting is done here and written to EMISSION (ALBEDO = 0) so Godot's own
/// DirectionalLight / ambient never double-light it and the night side stays
/// genuinely dark. `render_mode unshaded` is deliberately NOT used -- that path
/// drops EMISSION.

#include "res://shaders/planet_surface_lookup.gdshaderinc"

// --- Per-body uniforms, pushed by celestial_body_preview_runtime.gd ----------
uniform vec3  u_sun_dir = vec3(1.0, 0.0, 0.0);            // unit: body centre -> its star
uniform vec3  u_albedo : source_color = vec3(0.42, 0.45, 0.50);
uniform float u_body_radius_m = 1.0;                      // for relief-normal scale
uniform float u_ambient_floor = 0.05;                     // lit fraction on the day side at grazing sun
uniform float u_night_emission = 0.004;                   // faint star-lit glow on the dark side

uniform sampler2DArray u_relief_tex : filter_linear, repeat_disable;
uniform float u_relief_face_res = 0.0;
uniform float u_relief_ready = 0.0;                        // 0 -> smooth sphere
uniform float u_relief_shade_gain = 2.2;                   // exaggeration so distant relief stays readable

uniform vec3  u_atmo_color : source_color = vec3(0.35, 0.52, 0.92);
uniform float u_atmo_strength = 0.0;                       // 0 for an airless body

// --- Gas-giant band pattern (GasGiantModel.build_coverage_image) -------------
// Equirect map: R = zone value (0 belt .. 1 zone), G = detail, B = storm oval.
// When ready, the day-side colour is the belt/zone palette instead of a flat
// albedo, rotated by u_band_phase, with a faint band-gradient normal so the
// terminator wraps the belts.
uniform sampler2D u_band_tex : filter_linear, repeat_enable;
uniform float u_band_ready = 0.0;
uniform float u_band_phase = 0.0;
uniform vec3  u_band_zone : source_color = vec3(0.60, 0.66, 0.42);
uniform vec3  u_band_belt : source_color = vec3(0.34, 0.30, 0.18);

varying vec3 v_local_dir;   // unit direction on the base sphere (== world normal: root is unrotated + uniform scale)
varying vec3 v_world;

void vertex() {
	v_local_dir = normalize(VERTEX);
	v_world = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}

// Relief-perturbed normal from finite differences of the elevation cube texture.
vec3 rot_y(vec3 v, float a) {
	float c = cos(a);
	float s = sin(a);
	return vec3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

vec2 equirect(vec3 d) {
	return vec2(atan(d.z, d.x) / 6.28318530718 + 0.5, asin(clamp(d.y, -1.0, 1.0)) / 3.14159265359 + 0.5);
}

vec3 relief_normal(vec3 d, out float rel_h) {
	rel_h = 0.0;
	if (u_relief_ready < 0.5 || u_relief_face_res <= 0.5) {
		return d;
	}
	vec3 up_ref = abs(d.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	vec3 t = normalize(cross(up_ref, d));
	vec3 b = cross(d, t);
	float eps = 1.5 / u_relief_face_res;                   // ~1.5 texels, in direction space
	float h  = surface_macro_height(u_relief_tex, d, u_relief_face_res);
	float hx = surface_macro_height(u_relief_tex, normalize(d + t * eps), u_relief_face_res);
	float hy = surface_macro_height(u_relief_tex, normalize(d + b * eps), u_relief_face_res);
	rel_h = h;
	float arc = max(eps * max(u_body_radius_m, 1.0), 1.0);
	float k = u_relief_shade_gain / arc;
	return normalize(d - t * ((hx - h) * k) - b * ((hy - h) * k));
}

void fragment() {
	vec3 d = normalize(v_local_dir);
	float rel_h;
	vec3 n = relief_normal(d, rel_h);
	vec3 s = normalize(u_sun_dir);

	// Gas-giant bands: colour + a faint belt-gradient normal tilt for the terminator.
	vec3 base = u_albedo;
	if (u_band_ready > 0.5) {
		vec3 bd = rot_y(d, u_band_phase);
		vec2 uv = equirect(bd);
		vec4 bt = texture(u_band_tex, uv);
		// Keep the sharp belt edges the map already has (only a touch of AA).
		vec3 zone = mix(u_band_belt, u_band_zone, smoothstep(0.38, 0.62, bt.r));
		zone = mix(zone, min(zone * 1.7 + 0.10, vec3(1.0)), bt.b * 0.7);   // storm ovals proud
		zone *= 0.94 + 0.12 * bt.g;                                        // fine variation
		base = zone;
	} else if (u_relief_ready > 0.5) {
		float relief_ao = clamp(0.85 + rel_h * (0.00035), 0.6, 1.15);
		base *= relief_ao;
	}

	float ndl = dot(n, s);
	float lit = smoothstep(-0.09, 0.11, ndl);              // soft terminator
	float direct = max(ndl, 0.0);

	vec3 day = base * (u_ambient_floor + (1.0 - u_ambient_floor) * direct);
	vec3 night = base * u_night_emission;
	vec3 col = mix(night, day, lit);

	// Atmosphere rim on the lit limb.
	if (u_atmo_strength > 0.0) {
		vec3 view_dir = normalize(CAMERA_POSITION_WORLD - v_world);
		float limb = pow(1.0 - max(dot(d, view_dir), 0.0), 3.5);
		float rim_lit = smoothstep(-0.25, 0.35, dot(d, s));
		col += u_atmo_color * (limb * rim_lit * u_atmo_strength);
	}

	ALBEDO = vec3(0.0);
	EMISSION = col;
	METALLIC = 0.0;
	ROUGHNESS = 1.0;
	SPECULAR = 0.0;
	AO = 1.0;
}

#[compute]
#version 450

// Batched ocean query kernel. Rendering and physics use the same finite-depth
// bands, shoaling, coastal refraction, approximate diffraction and breaker cap.
// Bathymetry itself is kept outside this local RenderingDevice: callers pass
// depth, landward tangent and signed distance to the zero-height coastline.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct Query {
    vec4 position_depth;
    vec4 coast_shore_dist;
};

struct Result {
    vec4 height_normal;
    vec4 normal_vx;
    vec4 velocity;
};

layout(set = 0, binding = 0, std430) readonly buffer Queries { Query q[]; };
layout(set = 0, binding = 1, std430) writeonly buffer Results { Result r[]; };
layout(set = 0, binding = 2, std430) readonly buffer Params {
    float planet_radius;
    float wave_scale;
    float query_count;
    float time_s;
} params;

const float TAU_ = 6.283185307179586;
const float G = 9.81;
const float BREAKER_GAMMA = 0.78;
const vec3 SWELL_AXIS_A = vec3(0.827, 0.201, 0.525);
const vec3 SWELL_AXIS_B = vec3(-0.436, 0.331, 0.837);

vec3 safe_tangent(vec3 up, vec3 d) {
    vec3 t = d - up * dot(d, up);
    float l2 = dot(t, t);
    if (l2 < 1e-8) {
        vec3 a = abs(up.y) < 0.85 ? vec3(0,1,0) : vec3(1,0,0);
        t = cross(a, up);
        l2 = max(dot(t, t), 1e-8);
    }
    return t * inversesqrt(l2);
}

float geodesic_phase_coord(vec3 up, vec3 axis) {
    float mu = clamp(dot(normalize(up), normalize(axis)), -0.999999, 0.999999);
    return params.planet_radius * asin(mu);
}

float dispersion(float k, float depth) {
    return sqrt(max(G * k * tanh(k * max(depth, 0.05)), 1e-5));
}

float shoal_gain(float depth, float wavelength) {
    float qd = clamp(depth / max(wavelength * 0.20, 0.1), 0.0, 1.0);
    return mix(1.42, 1.0, smoothstep(0.08, 0.75, qd));
}

float coast_frequency_scale(float depth, float wavelength) {
    float transition_depth = max(12.0, wavelength * 0.32);
    float deep_blend = smoothstep(0.75, transition_depth, depth);
    return mix(0.56, 1.0, deep_blend);
}

float diffraction_weight(vec3 up, vec3 incident, vec3 landward, float depth) {
    vec3 i = safe_tangent(up, incident);
    vec3 l = safe_tangent(up, landward);
    float oblique = 1.0 - abs(dot(i, l));
    float coastal = 1.0 - smoothstep(12.0, 170.0, depth);
    return coastal * smoothstep(0.12, 0.88, oblique);
}

vec3 diffraction_direction(vec3 up, vec3 incident, vec3 landward) {
    vec3 l = safe_tangent(up, landward);
    vec3 coast_tangent = normalize(cross(up, l));
    float side = dot(safe_tangent(up, incident), coast_tangent) >= 0.0 ? 1.0 : -1.0;
    return safe_tangent(up, l + coast_tangent * side * 0.72);
}

void add_wave(vec3 up, vec3 dir, float offshore_phase_coord, float shore_distance,
              float depth, float wavelength, float base_amp, float steepness,
              float phase_off, inout vec3 displacement, inout vec3 grad,
              inout vec3 vel, inout float breaking) {
    if (depth <= 0.01) return;
    float k = TAU_ / wavelength;
    float omega = dispersion(k, depth) * coast_frequency_scale(depth, wavelength);
    float raw_amp = base_amp * shoal_gain(depth, wavelength) * params.wave_scale;
    float breaker_amp = max(0.015, BREAKER_GAMMA * depth * 0.5);
    breaking = max(breaking,
        smoothstep(0.72, 1.08, raw_amp / max(breaker_amp, 0.015))
        * (1.0 - smoothstep(0.3, 8.0, depth)));
    float amp = min(raw_amp, breaker_amp);
    vec3 travel = safe_tangent(up, dir);
    float shore_blend = (1.0 - smoothstep(18.0, 150.0, depth)) * 0.94;
    float phase_coord = mix(offshore_phase_coord, shore_distance, shore_blend);
    float phase = phase_coord * k - omega * params.time_s + phase_off;
    float s = sin(phase);
    float c = cos(phase);
    float gerstner_q = min(steepness / max(k * max(amp, 0.01), 0.01), 0.95);
    float horizontal = gerstner_q * amp;
    displacement += up * (amp * s) + travel * (horizontal * c);
    grad += travel * (amp * k * c);
    vel += up * (-amp * omega * c) + travel * (horizontal * omega * s);
}

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (float(i) >= params.query_count) return;

    vec3 p = q[i].position_depth.xyz;
    float depth = max(q[i].position_depth.w, 0.0);
    vec3 up = normalize(p);
    vec3 landward = safe_tangent(up, q[i].coast_shore_dist.xyz);
    float shore_distance = q[i].coast_shore_dist.w;

    vec3 swell_a = safe_tangent(up, SWELL_AXIS_A);
    vec3 swell_b = safe_tangent(up, SWELL_AXIS_B);
    float refract = 1.0 - smoothstep(22.0, 150.0, depth);
    vec3 refracted_a = normalize(mix(swell_a, landward, refract));
    vec3 refracted_b = normalize(mix(swell_b, landward, refract * 0.88));
    float diffract_a = diffraction_weight(up, swell_a, landward, depth);
    float diffract_b = diffraction_weight(up, swell_b, landward, depth) * 0.82;
    vec3 dir_a = normalize(mix(refracted_a, diffraction_direction(up, swell_a, landward), diffract_a * 0.52));
    vec3 dir_b = normalize(mix(refracted_b, diffraction_direction(up, swell_b, landward), diffract_b * 0.46));

    // Same constant-metric spherical phase map as the render shader.
    float phase_a = geodesic_phase_coord(up, SWELL_AXIS_A);
    float phase_b = geodesic_phase_coord(up, SWELL_AXIS_B);

    vec3 displacement = vec3(0.0);
    vec3 grad = vec3(0.0);
    vec3 vel = vec3(0.0);
    float breaking = 0.0;
    add_wave(up, dir_a, phase_a, shore_distance, depth, 96.0, 0.78, 0.62, 0.0,
        displacement, grad, vel, breaking);
    add_wave(up, dir_b, phase_b, shore_distance, depth, 42.0, 0.32, 0.55, 1.7,
        displacement, grad, vel, breaking);
    add_wave(up, dir_a, phase_a, shore_distance, depth, 18.0, 0.105, 0.48, 3.1,
        displacement, grad, vel, breaking);
    add_wave(up, dir_b, phase_b, shore_distance, depth, 8.0, 0.036, 0.42, 4.6,
        displacement, grad, vel, breaking);

    vec3 normal = normalize(up - grad);
    float height = dot(displacement, up);
    r[i].height_normal = vec4(height, normal.x, normal.y, normal.z);
    r[i].normal_vx = vec4(normal, vel.x);
    r[i].velocity = vec4(vel, breaking);
}

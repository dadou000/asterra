#[compute]
#version 450

// Batched ocean query kernel. One invocation evaluates one buoyancy sample.
// CPU callers provide planet-space positions and local depth; the GPU returns
// surface position/normal/velocity so many pontoons/hull probes can share one
// dispatch instead of repeating wave math in GDScript.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct Query {
    vec4 position_depth; // xyz planet position, w still-water depth
    vec4 coast_dir_time; // xyz landward tangent (or swell fallback), w time
};

struct Result {
    vec4 surface_height_normal_x; // x height offset, yzw normal xyz packed oddly below
    vec4 normal_velocity_x;       // xyz normal, w velocity x
    vec4 velocity;                // xyz velocity, w breaking
};

layout(set = 0, binding = 0, std430) readonly buffer Queries { Query q[]; };
layout(set = 0, binding = 1, std430) writeonly buffer Results { Result r[]; };
layout(set = 0, binding = 2, std430) readonly buffer Params {
    float planet_radius;
    float wave_scale;
    uint query_count;
    float pad0;
} params;

const float PI2 = 6.283185307179586;
const float G = 9.81;

vec3 tangent(vec3 up, vec3 d) {
    vec3 t = d - up * dot(d, up);
    float l = length(t);
    if (l < 1e-5) {
        vec3 a = abs(up.y) < 0.85 ? vec3(0,1,0) : vec3(1,0,0);
        t = cross(a, up);
        l = max(length(t), 1e-5);
    }
    return t / l;
}

void add_wave(vec3 p, vec3 up, vec3 dir, float depth, float wavelength,
              float base_amp, float phase_off, float time_s,
              inout float h, inout vec3 grad, inout vec3 vel, inout float breaking) {
    float k = PI2 / wavelength;
    float omega = sqrt(max(G * k * tanh(k * max(depth, 0.05)), 1e-5));
    float shoal = mix(1.42, 1.0, smoothstep(0.08, 0.75, depth / max(wavelength * 0.20, 0.1)));
    float raw_amp = base_amp * shoal * params.wave_scale;
    float limit = max(0.015, 0.78 * depth * 0.5);
    breaking = max(breaking, smoothstep(0.72, 1.08, raw_amp / max(limit, 0.015)) * (1.0 - smoothstep(0.3, 8.0, depth)));
    float amp = min(raw_amp, limit);
    vec3 d = tangent(up, dir);
    float ph = dot(p, d) * k - omega * time_s + phase_off;
    float s = sin(ph), c = cos(ph);
    h += amp * s;
    grad += d * (amp * k * c);
    vel += up * (-amp * omega * c);
}

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= params.query_count) return;
    vec3 p = q[i].position_depth.xyz;
    float depth = max(q[i].position_depth.w, 0.0);
    vec3 up = normalize(p);
    vec3 dir = tangent(up, q[i].coast_dir_time.xyz);
    float time_s = q[i].coast_dir_time.w;

    float h = 0.0;
    vec3 grad = vec3(0.0);
    vec3 vel = vec3(0.0);
    float breaking = 0.0;
    add_wave(p, up, dir, depth, 96.0, 0.78, 0.0, time_s, h, grad, vel, breaking);
    add_wave(p, up, tangent(up, vec3(-0.436, 0.331, 0.837)), depth, 42.0, 0.32, 1.7, time_s, h, grad, vel, breaking);
    add_wave(p, up, dir, depth, 18.0, 0.105, 3.1, time_s, h, grad, vel, breaking);
    add_wave(p, up, tangent(up, vec3(-0.703, -0.264, 0.660)), depth, 8.0, 0.036, 4.6, time_s, h, grad, vel, breaking);

    vec3 normal = normalize(up - grad);
    r[i].surface_height_normal_x = vec4(h, normal.x, normal.y, normal.z);
    r[i].normal_velocity_x = vec4(normal, vel.x);
    r[i].velocity = vec4(vel, breaking);
}

#[compute]
#version 450

// Reconstructs the fixed-domain conservative SWE state into the shared visible
// water cache. The solver stores absolute hydraulic state; the render cache stores
// a surface-height value relative to the configured datum plus physical depth.
//
// Output RGBA:
//   R = eta - reference_surface [m]
//   G = tangent velocity X [m/s]
//   B = tangent velocity Y [m/s]
//   A = physical water depth [m]

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer HydroState {
    vec4 cells[]; // h, hu, hv, bed
};
layout(rgba32f, set = 0, binding = 1) uniform writeonly image2D surface_out;
layout(set = 0, binding = 2, std430) readonly buffer Params {
    vec4 source_grid;   // width, height, dx, dry_eps
    vec4 source_frame;  // source center plane x/y, reference surface, reserved
    vec4 target_frame;  // target resolution, half extent, target center x/y
} params;

int source_width() { return int(params.source_grid.x + 0.5); }
int source_height() { return int(params.source_grid.y + 0.5); }
float source_dx() { return max(params.source_grid.z, 1e-4); }
float dry_eps() { return max(params.source_grid.w, 1e-8); }

int idx(ivec2 p) {
    return p.y * source_width() + p.x;
}

vec4 state_at(ivec2 p) {
    ivec2 q = clamp(p, ivec2(0), ivec2(source_width() - 1, source_height() - 1));
    return cells[idx(q)];
}

vec4 bilinear_state(vec2 source_cell) {
    vec2 base_f = floor(source_cell);
    ivec2 p00 = ivec2(base_f);
    vec2 f = source_cell - base_f;
    vec4 a = mix(state_at(p00), state_at(p00 + ivec2(1, 0)), f.x);
    vec4 b = mix(state_at(p00 + ivec2(0, 1)), state_at(p00 + ivec2(1, 1)), f.x);
    return mix(a, b, f.y);
}

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    int resolution = int(params.target_frame.x + 0.5);
    if (pixel.x >= resolution || pixel.y >= resolution) return;

    float half_extent = max(params.target_frame.y, 1.0);
    vec2 target_center = params.target_frame.zw;
    vec2 uv = (vec2(pixel) + vec2(0.5)) / float(resolution);
    vec2 plane_m = target_center + (uv * 2.0 - vec2(1.0)) * half_extent;

    vec2 source_center = params.source_frame.xy;
    vec2 source_local_m = plane_m - source_center;
    vec2 source_cell = source_local_m / source_dx()
        + vec2(float(source_width()), float(source_height())) * 0.5 - vec2(0.5);

    // Bilinear interpolation requires a complete 2x2 footprint. Everything
    // outside the active fixed domain is an empty render-cache texel.
    if (source_cell.x < 0.0 || source_cell.y < 0.0
            || source_cell.x > float(source_width() - 1)
            || source_cell.y > float(source_height() - 1)) {
        imageStore(surface_out, pixel, vec4(0.0));
        return;
    }

    vec4 s = bilinear_state(source_cell);
    bool invalid = any(isnan(s)) || any(isinf(s));
    float h = invalid ? 0.0 : max(s.x, 0.0);
    float wet = smoothstep(dry_eps(), dry_eps() * 4.0, h);
    if (wet <= 0.0) {
        imageStore(surface_out, pixel, vec4(0.0));
        return;
    }

    float eta = s.w + h;
    float reference_surface = params.source_frame.z;
    float surface_height = (eta - reference_surface) * wet;
    vec2 velocity = s.yz / max(h, dry_eps());
    velocity *= wet;
    float depth = h * wet;
    imageStore(surface_out, pixel, vec4(surface_height, velocity, depth));
}

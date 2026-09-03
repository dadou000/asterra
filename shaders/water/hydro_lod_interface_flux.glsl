#[compute]
#version 450

// Conservative 2:1 coarse/fine interface correction.
//
// The ordinary sparse SWE pass deliberately sees a missing same-level neighbor and
// therefore applies its proven reflective boundary contribution. This pass replaces
// that contribution with two fine-segment hydrostatic Riemann fluxes:
//
//   one coarse edge cell <-> two fine edge cells
//
// Integrated mass is identical and opposite on both representations. Flat-bed
// momentum is likewise exactly conservative; hydrostatic reconstruction contributes
// the expected bed-pressure source on each side. Lake-at-rest remains unchanged.
//
// One dispatch handles exactly one interface descriptor. The CPU records descriptors
// serially with barriers, so in-place state updates cannot race at LOD corners.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) buffer State {
    vec4 cells[]; // h, hu, hv, bed
};
layout(set = 0, binding = 1, std430) readonly buffer Occupancy {
    int occupied[];
};
layout(set = 0, binding = 2, std430) readonly buffer Interfaces {
    uvec4 interface_words[]; // two uvec4 rows per descriptor
};
layout(set = 0, binding = 3, std430) readonly buffer Params {
    uvec4 grid;    // tile_res, capacity, interface_count, H0 tile level
    vec4 physics;  // H0 dx, gravity, dry_eps, HydroLOD enabled
} params;
layout(set = 0, binding = 4, std430) readonly buffer Control {
    uint pre_max_speed_bits;
    uint pre_max_depth_bits;
    uint pre_wet_count;
    uint pre_invalid_count;

    uint post_max_speed_bits;
    uint post_max_depth_bits;
    uint post_wet_count;
    uint post_invalid_count;

    float requested_dt;
    float remaining_dt;
    float current_dt;
    float advanced_dt;

    float min_cfl_dt;
    float last_cfl_dt;
    uint steps_taken;
    uint max_substeps;

    uint cfl_clamped;
    uint iteration_active;
    uint iter_max_speed_bits;
    uint iter_max_depth_bits;

    uint iter_wet_count;
    uint iter_invalid_count;
    uint reserved0;
    uint reserved1;

    uint iter_max_cfl_rate_bits;
    uint reserved2;
} control;
layout(set = 0, binding = 5, std430) readonly buffer TileMetadata {
    ivec4 tile_metadata[];
};

layout(push_constant, std430) uniform InterfacePush {
    uint interface_index;
    uint pad0;
    uint pad1;
    uint pad2;
} pc;

const uint DIR_WEST = 0u;
const uint DIR_EAST = 1u;
const uint DIR_SOUTH = 2u;
const uint DIR_NORTH = 3u;

uint tile_res() { return max(params.grid.x, 1u); }
uint capacity() { return max(params.grid.y, 1u); }
uint cells_per_tile() { uint r = tile_res(); return r * r; }
uint idx(uint slot, uvec2 p) { return slot * cells_per_tile() + p.y * tile_res() + p.x; }
float grav() { return max(params.physics.y, 1e-4); }
float dry_eps() { return max(params.physics.z, 1e-8); }

vec2 edge_normal(uint direction) {
    if (direction == DIR_WEST) return vec2(-1.0, 0.0);
    if (direction == DIR_EAST) return vec2( 1.0, 0.0);
    if (direction == DIR_SOUTH) return vec2(0.0, -1.0);
    return vec2(0.0, 1.0);
}

vec2 edge_tangent(uint direction) {
    if (direction == DIR_WEST || direction == DIR_EAST)
        return vec2(0.0, 1.0);
    return vec2(1.0, 0.0);
}

uvec2 edge_cell(uint direction, uint k) {
    uint r = tile_res();
    if (direction == DIR_WEST) return uvec2(0u, k);
    if (direction == DIR_EAST) return uvec2(r - 1u, k);
    if (direction == DIR_SOUTH) return uvec2(k, 0u);
    return uvec2(k, r - 1u);
}

vec2 destination_to_source(vec2 q_dest, uint source_direction,
        uint destination_direction, bool reversed) {
    vec2 ns = edge_normal(source_direction);
    vec2 ts = edge_tangent(source_direction);
    vec2 nd = edge_normal(destination_direction);
    vec2 td = edge_tangent(destination_direction);
    float qn = dot(q_dest, nd);
    float qt = dot(q_dest, td);
    float orientation = reversed ? -1.0 : 1.0;
    return (-qn) * ns + (orientation * qt) * ts;
}

vec2 source_to_destination(vec2 q_source, uint source_direction,
        uint destination_direction, bool reversed) {
    vec2 ns = edge_normal(source_direction);
    vec2 ts = edge_tangent(source_direction);
    vec2 nd = edge_normal(destination_direction);
    vec2 td = edge_tangent(destination_direction);
    float qn = dot(q_source, ns);
    float qt = dot(q_source, ts);
    float orientation = reversed ? -1.0 : 1.0;
    return (-qn) * nd + (orientation * qt) * td;
}

vec3 reconstruct(vec3 q, float reconstructed_h) {
    if (q.x <= dry_eps() || reconstructed_h <= dry_eps()) return vec3(0.0);
    float scale = reconstructed_h / q.x;
    return vec3(reconstructed_h, q.yz * scale);
}

vec3 normal_flux(vec3 q) {
    if (q.x <= dry_eps()) return vec3(0.0);
    float un = q.y / q.x;
    return vec3(q.y,
        q.y * un + 0.5 * grav() * q.x * q.x,
        q.z * un);
}

float normal_signal(vec3 q) {
    if (q.x <= dry_eps()) return 0.0;
    return abs(q.y / q.x) + sqrt(grav() * q.x);
}

vec3 rusanov(vec3 left, vec3 right) {
    float a = max(normal_signal(left), normal_signal(right));
    return 0.5 * (normal_flux(left) + normal_flux(right))
        - 0.5 * a * (right - left);
}

vec3 interface_flux(vec4 coarse, vec4 fine_in_coarse,
        out float h_coarse_recon, out float h_fine_recon) {
    float zstar = max(coarse.w, fine_in_coarse.w);
    h_coarse_recon = max(coarse.x + coarse.w - zstar, 0.0);
    h_fine_recon = max(fine_in_coarse.x + fine_in_coarse.w - zstar, 0.0);
    vec3 qc = vec3(coarse.x, coarse.y, coarse.z);
    vec3 qf = vec3(fine_in_coarse.x, fine_in_coarse.y, fine_in_coarse.z);
    return rusanov(reconstruct(qc, h_coarse_recon),
        reconstruct(qf, h_fine_recon));
}

void main() {
    if (control.iteration_active == 0u || control.current_dt <= 0.0
            || pc.interface_index >= params.grid.z) return;

    uint lane = gl_LocalInvocationIndex;
    uint r = tile_res();
    if (lane >= r) return;

    uvec4 a = interface_words[pc.interface_index * 2u + 0u];
    uvec4 b = interface_words[pc.interface_index * 2u + 1u];
    uint coarse_slot = a.x;
    uint fine_low_slot = a.y;
    uint fine_high_slot = a.z;
    uint coarse_direction = a.w;
    uint fine_direction = b.x;
    bool reversed = b.y != 0u;
    if (coarse_slot >= capacity() || fine_low_slot >= capacity()
            || fine_high_slot >= capacity() || coarse_direction > 3u
            || fine_direction > 3u || occupied[coarse_slot] == 0
            || occupied[fine_low_slot] == 0 || occupied[fine_high_slot] == 0)
        return;

    int base_level = int(params.grid.w);
    int coarse_level = tile_metadata[coarse_slot].y;
    int fine_low_level = tile_metadata[fine_low_slot].y;
    int fine_high_level = tile_metadata[fine_high_slot].y;
    if (coarse_level < 0 || fine_low_level != coarse_level + 1
            || fine_high_level != coarse_level + 1) return;

    float coarse_dx = max(params.physics.x, 1e-4)
        * exp2(float(base_level - coarse_level));
    float fine_dx = coarse_dx * 0.5;
    float dt = control.current_dt;

    uvec2 coarse_p = edge_cell(coarse_direction, lane);
    uint coarse_i = idx(coarse_slot, coarse_p);
    vec4 coarse = cells[coarse_i];
    if (any(isnan(coarse)) || any(isinf(coarse))) return;

    vec2 nc = edge_normal(coarse_direction);
    vec2 tc = edge_tangent(coarse_direction);
    vec2 coarse_q = coarse.yz;
    float coarse_h = max(coarse.x, 0.0);
    float coarse_qn = dot(coarse_q, nc);
    float coarse_qt = dot(coarse_q, tc);
    vec3 coarse_nt = vec3(coarse_h, coarse_qn, coarse_qt);

    // The normal solver already applied a reflective boundary. Its net boundary
    // momentum contribution after hydrostatic correction is -qn^2/h along outward
    // normal. Remove that once, then accumulate the two physical fine segments.
    float wall_normal_rate = 0.0;
    if (coarse_h > dry_eps())
        wall_normal_rate = -(coarse_qn * coarse_qn / coarse_h) * dt / coarse_dx;
    vec3 coarse_delta_nt = vec3(0.0, -wall_normal_rate, 0.0);

    for (uint sub = 0u; sub < 2u; ++sub) {
        uint source_global = lane * 2u + sub;
        uint destination_global = reversed
            ? (2u * r - 1u - source_global) : source_global;
        uint fine_slot = destination_global < r ? fine_low_slot : fine_high_slot;
        uint fine_k = destination_global % r;
        uvec2 fine_p = edge_cell(fine_direction, fine_k);
        uint fine_i = idx(fine_slot, fine_p);
        vec4 fine = cells[fine_i];
        if (any(isnan(fine)) || any(isinf(fine))) continue;

        vec2 fine_q_in_coarse = destination_to_source(fine.yz,
            coarse_direction, fine_direction, reversed);
        vec4 fine_common = vec4(max(fine.x, 0.0), fine_q_in_coarse, fine.w);
        float hl;
        float hr;
        vec3 flux = interface_flux(vec4(coarse_h, coarse_q, coarse.w),
            fine_common, hl, hr);

        float coarse_segment_scale = dt * fine_dx / (coarse_dx * coarse_dx);
        coarse_delta_nt.x += -flux.x * coarse_segment_scale;
        coarse_delta_nt.y += (-flux.y + 0.5 * grav() * hl * hl)
            * coarse_segment_scale;
        coarse_delta_nt.z += -flux.z * coarse_segment_scale;

        // Fine desired cross-interface contribution in the common coarse frame.
        // Convert its physical momentum increment into the fine tile local frame.
        float fine_scale = dt / fine_dx;
        float fine_mass_delta = flux.x * fine_scale;
        vec2 fine_momentum_common = nc
            * ((flux.y - 0.5 * grav() * hr * hr) * fine_scale)
            + tc * (flux.z * fine_scale);
        vec2 fine_momentum_delta = source_to_destination(fine_momentum_common,
            coarse_direction, fine_direction, reversed);

        // Remove the reflective-wall impulse already applied to this fine cell.
        vec2 nf = edge_normal(fine_direction);
        float fine_h = max(fine.x, 0.0);
        float fine_qn = dot(fine.yz, nf);
        if (fine_h > dry_eps()) {
            vec2 fine_wall = nf * (-(fine_qn * fine_qn / fine_h) * fine_scale);
            fine_momentum_delta -= fine_wall;
        }

        vec4 fine_updated = fine;
        fine_updated.x = max(fine_updated.x + fine_mass_delta, 0.0);
        fine_updated.yz += fine_momentum_delta;
        if (fine_updated.x <= dry_eps()) fine_updated.xyz = vec3(0.0);
        cells[fine_i] = fine_updated;
    }

    vec4 coarse_updated = coarse;
    coarse_updated.x = max(coarse_updated.x + coarse_delta_nt.x, 0.0);
    coarse_updated.yz += nc * coarse_delta_nt.y + tc * coarse_delta_nt.z;
    if (coarse_updated.x <= dry_eps()) coarse_updated.xyz = vec3(0.0);
    cells[coarse_i] = coarse_updated;
}

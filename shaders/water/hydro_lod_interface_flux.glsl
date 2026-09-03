#[compute]
#version 450

// Conservative 2:1 coarse/fine interface correction.
//
// The ordinary sparse SWE pass sees no same-level neighbor at a mixed-resolution
// edge, so it advances that edge as reflective. This pass runs immediately after
// A->B SWE and before B->A canonicalization. It reads the same pre-step A state,
// removes the reflective contribution already present in B, and substitutes one
// physical coarse/fine flux integrated over the two fine edge segments.
//
// One coarse edge cell maps to two fine edge cells. Integrated mass transfer is
// identical and opposite. Momentum is transformed through edge normal/tangent
// frames, including cube-face seam reversal. A positivity limiter scales only the
// physical interface transfer when a donor lacks the requested post-step water;
// the same scale is applied to both sides of that segment.
//
// Descriptors are dispatched serially with barriers by HydroLODInterfaceFluxGPU,
// avoiding in-place B races at tile corners.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer StateIn {
    vec4 cells_in[]; // canonical pre-step A: h, hu, hv, bed
};
layout(set = 0, binding = 1, std430) buffer StateOut {
    vec4 cells_out[]; // ordinary post-step B, corrected in-place
};
layout(set = 0, binding = 2, std430) readonly buffer Occupancy {
    int occupied[];
};
layout(set = 0, binding = 3, std430) readonly buffer Interfaces {
    uvec4 interface_words[]; // two uvec4 rows per descriptor
};
layout(set = 0, binding = 4, std430) readonly buffer Params {
    uvec4 grid;    // tile_res, capacity, interface_count, H0 tile level
    vec4 physics;  // H0 dx, gravity, dry_eps, enabled
} params;
layout(set = 0, binding = 5, std430) readonly buffer Control {
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
layout(set = 0, binding = 6, std430) readonly buffer TileMetadata {
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
const uint INVALID_SLOT = 0xffffffffu;

uint tile_res() { return max(params.grid.x, 1u); }
uint capacity() { return max(params.grid.y, 1u); }
uint cells_per_tile() { uint r = tile_res(); return r * r; }
uint idx(uint slot, uvec2 p) { return slot * cells_per_tile() + p.y * tile_res() + p.x; }
float grav() { return max(params.physics.y, 1e-4); }
float dry_eps() { return max(params.physics.z, 1e-8); }

bool live_slot(uint slot) {
    return slot != INVALID_SLOT && slot < capacity() && occupied[slot] != 0;
}

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

// Destination local (+u,+v) -> source local (+u,+v) for the same physical tangent
// vector. Outward normals oppose each other; edge tangents may reverse at a seam.
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

// Source local (+u,+v) -> destination local (+u,+v) for the same physical vector.
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

// q = (h, q_normal, q_tangent) in the coarse outward-normal frame.
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

vec3 interface_flux(vec4 coarse_nt, vec4 fine_nt,
        out float h_coarse_recon, out float h_fine_recon) {
    float zstar = max(coarse_nt.w, fine_nt.w);
    h_coarse_recon = max(coarse_nt.x + coarse_nt.w - zstar, 0.0);
    h_fine_recon = max(fine_nt.x + fine_nt.w - zstar, 0.0);
    return rusanov(reconstruct(coarse_nt.xyz, h_coarse_recon),
        reconstruct(fine_nt.xyz, h_fine_recon));
}

void main() {
    if (params.physics.w < 0.5 || control.iteration_active == 0u
            || control.current_dt <= 0.0 || pc.interface_index >= params.grid.z)
        return;

    uint lane = gl_GlobalInvocationID.x;
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
    if (!live_slot(coarse_slot) || coarse_direction > 3u || fine_direction > 3u)
        return;
    bool low_live = live_slot(fine_low_slot);
    bool high_live = live_slot(fine_high_slot);
    if (!low_live && !high_live) return;

    int base_level = int(params.grid.w);
    int coarse_level = tile_metadata[coarse_slot].y;
    if (coarse_level < 0 || coarse_level > base_level) return;
    if (low_live && tile_metadata[fine_low_slot].y != coarse_level + 1) return;
    if (high_live && tile_metadata[fine_high_slot].y != coarse_level + 1) return;

    float coarse_dx = max(params.physics.x, 1e-4)
        * exp2(float(base_level - coarse_level));
    float fine_dx = coarse_dx * 0.5;
    float coarse_area = coarse_dx * coarse_dx;
    float fine_area = fine_dx * fine_dx;
    float dt = control.current_dt;

    uvec2 coarse_p = edge_cell(coarse_direction, lane);
    uint coarse_i = idx(coarse_slot, coarse_p);
    vec4 coarse_before = cells_in[coarse_i];
    vec4 coarse_after = cells_out[coarse_i];
    if (any(isnan(coarse_before)) || any(isinf(coarse_before))
            || any(isnan(coarse_after)) || any(isinf(coarse_after))) return;

    vec2 nc = edge_normal(coarse_direction);
    vec2 tc = edge_tangent(coarse_direction);
    float coarse_h = max(coarse_before.x, 0.0);
    float coarse_qn = dot(coarse_before.yz, nc);
    float coarse_qt = dot(coarse_before.yz, tc);
    vec4 coarse_nt = vec4(coarse_h, coarse_qn, coarse_qt, coarse_before.w);

    // Per fine segment data. Integrated volume is positive coarse -> fine.
    bool valid[2];
    uint fine_index[2];
    uint fine_slot_for_segment[2];
    float volume_m3[2];
    float scale_segment[2];
    vec3 coarse_desired_delta_nt[2];
    vec2 fine_desired_delta_local[2];
    vec2 fine_wall_delta_local[2];

    for (uint sub = 0u; sub < 2u; ++sub) {
        valid[sub] = false;
        fine_index[sub] = 0u;
        fine_slot_for_segment[sub] = INVALID_SLOT;
        volume_m3[sub] = 0.0;
        scale_segment[sub] = 1.0;
        coarse_desired_delta_nt[sub] = vec3(0.0);
        fine_desired_delta_local[sub] = vec2(0.0);
        fine_wall_delta_local[sub] = vec2(0.0);

        uint source_global = lane * 2u + sub;
        uint destination_global = reversed
            ? (2u * r - 1u - source_global) : source_global;
        uint fine_slot = destination_global < r ? fine_low_slot : fine_high_slot;
        if (!live_slot(fine_slot)) continue;
        uint fine_k = destination_global % r;
        uint fi = idx(fine_slot, edge_cell(fine_direction, fine_k));
        vec4 fine_before = cells_in[fi];
        vec4 fine_after = cells_out[fi];
        if (any(isnan(fine_before)) || any(isinf(fine_before))
                || any(isnan(fine_after)) || any(isinf(fine_after))) continue;

        vec2 fine_q_source = destination_to_source(fine_before.yz,
            coarse_direction, fine_direction, reversed);
        float fine_h = max(fine_before.x, 0.0);
        vec4 fine_nt = vec4(fine_h,
            dot(fine_q_source, nc), dot(fine_q_source, tc), fine_before.w);

        float hc;
        float hf;
        vec3 flux = interface_flux(coarse_nt, fine_nt, hc, hf);
        float segment_volume = flux.x * dt * fine_dx;
        volume_m3[sub] = segment_volume;

        float coarse_scale = dt * fine_dx / coarse_area;
        coarse_desired_delta_nt[sub] = vec3(
            -flux.x * coarse_scale,
            (-flux.y + 0.5 * grav() * hc * hc) * coarse_scale,
            -flux.z * coarse_scale);

        float fine_scale = dt * fine_dx / fine_area; // == dt/fine_dx
        vec2 desired_common_local = nc
            * ((flux.y - 0.5 * grav() * hf * hf) * fine_scale)
            + tc * (flux.z * fine_scale);
        fine_desired_delta_local[sub] = source_to_destination(
            desired_common_local, coarse_direction, fine_direction, reversed);

        vec2 nf = edge_normal(fine_direction);
        float fine_qn = dot(fine_before.yz, nf);
        if (fine_h > dry_eps()) {
            fine_wall_delta_local[sub] = nf
                * (-(fine_qn * fine_qn / fine_h) * dt / fine_dx);
        }

        valid[sub] = true;
        fine_index[sub] = fi;
        fine_slot_for_segment[sub] = fine_slot;
    }

    // First limit any fine -> coarse segment independently by that fine donor's
    // post-wall water. Then let actual incoming fine volume augment what the coarse
    // donor may send outward over its positive segments.
    float incoming_to_coarse = 0.0;
    float outgoing_from_coarse = 0.0;
    for (uint sub = 0u; sub < 2u; ++sub) {
        if (!valid[sub]) continue;
        float v = volume_m3[sub];
        if (v < 0.0) {
            float available = max(cells_out[fine_index[sub]].x, 0.0) * fine_area;
            scale_segment[sub] = min(1.0, available / max(-v, 1e-20));
            incoming_to_coarse += (-v) * scale_segment[sub];
        } else {
            outgoing_from_coarse += v;
        }
    }
    if (outgoing_from_coarse > 0.0) {
        float available_coarse = max(coarse_after.x, 0.0) * coarse_area
            + incoming_to_coarse;
        float coarse_out_scale = min(1.0,
            available_coarse / max(outgoing_from_coarse, 1e-20));
        for (uint sub = 0u; sub < 2u; ++sub) {
            if (valid[sub] && volume_m3[sub] > 0.0)
                scale_segment[sub] = coarse_out_scale;
        }
    }

    // Remove the full reflective wall impulse already recorded for this coarse edge
    // cell. The two physical segment contributions then replace it.
    vec3 coarse_correction_nt = vec3(0.0);
    if (coarse_h > dry_eps()) {
        float coarse_wall_qn_delta = -(coarse_qn * coarse_qn / coarse_h)
            * dt / coarse_dx;
        coarse_correction_nt.y -= coarse_wall_qn_delta;
    }

    for (uint sub = 0u; sub < 2u; ++sub) {
        if (!valid[sub]) continue;
        float s = scale_segment[sub];
        float actual_volume = volume_m3[sub] * s;
        vec3 desired_coarse = coarse_desired_delta_nt[sub] * s;
        // Derive mass from the shared integrated parcel rather than separately
        // rounded flux expressions on each side.
        desired_coarse.x = -actual_volume / coarse_area;
        coarse_correction_nt += desired_coarse;

        uint fi = fine_index[sub];
        vec4 fine_after = cells_out[fi];
        fine_after.x += actual_volume / fine_area;
        fine_after.yz += fine_desired_delta_local[sub] * s
            - fine_wall_delta_local[sub];
        if (fine_after.x <= dry_eps()) {
            fine_after.x = max(fine_after.x, 0.0);
            fine_after.yz = vec2(0.0);
        }
        cells_out[fi] = fine_after;
    }

    coarse_after.x += coarse_correction_nt.x;
    coarse_after.yz += nc * coarse_correction_nt.y + tc * coarse_correction_nt.z;
    if (coarse_after.x <= dry_eps()) {
        coarse_after.x = max(coarse_after.x, 0.0);
        coarse_after.yz = vec2(0.0);
    }
    cells_out[coarse_i] = coarse_after;
}

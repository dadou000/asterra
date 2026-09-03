#[compute]
#version 450

// Conservative 2:1 interface coupling for temporal HydroLOD subcycling.
// Fine-side updates are applied whenever the fine level is due. Equal/opposite
// coarse increments accumulate in a per-interface flux register. When the coarse
// level reaches its synchronization tick, the accumulated register replaces the
// represented fraction of the coarse reflective wall contribution in one commit.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer StateIn { vec4 cells_in[]; };
layout(set = 0, binding = 1, std430) buffer StateOut { vec4 cells_out[]; };
layout(set = 0, binding = 2, std430) readonly buffer Occupancy { int occupied[]; };
layout(set = 0, binding = 3, std430) readonly buffer Interfaces { uvec4 interface_words[]; };
layout(set = 0, binding = 4, std430) readonly buffer Params {
    uvec4 grid;
    vec4 physics;
} params;
layout(set = 0, binding = 5, std430) readonly buffer Control {
    uint pre_max_speed_bits; uint pre_max_depth_bits; uint pre_wet_count; uint pre_invalid_count;
    uint post_max_speed_bits; uint post_max_depth_bits; uint post_wet_count; uint post_invalid_count;
    float requested_dt; float remaining_dt; float current_dt; float advanced_dt;
    float min_cfl_dt; float last_cfl_dt; uint steps_taken; uint max_substeps;
    uint cfl_clamped; uint iteration_active; uint iter_max_speed_bits; uint iter_max_depth_bits;
    uint iter_wet_count; uint iter_invalid_count; uint reserved0; uint reserved1;
    uint iter_max_cfl_rate_bits; uint reserved2;
    uint temporal_enabled; uint temporal_max_lod; uint temporal_fine_tick; uint temporal_force_sync;
    vec4 temporal_accum_0_3; vec4 temporal_accum_4_7;
    vec4 temporal_step_0_3; vec4 temporal_step_4_7;
} control;
layout(set = 0, binding = 6, std430) readonly buffer TileMetadata { ivec4 tile_metadata[]; };
layout(set = 0, binding = 7, std430) buffer FluxRegisters {
    vec4 coarse_delta_nt[]; // depth, normal momentum, tangent momentum, reserved
};

layout(push_constant, std430) uniform InterfacePush {
    uint interface_index; uint pad0; uint pad1; uint pad2;
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
bool live_slot(uint slot) { return slot != INVALID_SLOT && slot < capacity() && occupied[slot] != 0; }

float lod_dt(uint lod) {
    if (control.temporal_enabled == 0u) return max(control.current_dt, 0.0);
    lod = min(lod, min(control.temporal_max_lod, 7u));
    if (lod < 4u) return control.temporal_step_0_3[int(lod)];
    return control.temporal_step_4_7[int(lod - 4u)];
}

vec2 edge_normal(uint d) {
    if (d == DIR_WEST) return vec2(-1.0, 0.0);
    if (d == DIR_EAST) return vec2(1.0, 0.0);
    if (d == DIR_SOUTH) return vec2(0.0, -1.0);
    return vec2(0.0, 1.0);
}
vec2 edge_tangent(uint d) {
    return (d == DIR_WEST || d == DIR_EAST) ? vec2(0.0, 1.0) : vec2(1.0, 0.0);
}
uvec2 edge_cell(uint d, uint k) {
    uint r = tile_res();
    if (d == DIR_WEST) return uvec2(0u, k);
    if (d == DIR_EAST) return uvec2(r - 1u, k);
    if (d == DIR_SOUTH) return uvec2(k, 0u);
    return uvec2(k, r - 1u);
}
vec2 destination_to_source(vec2 q, uint sd, uint dd, bool reversed) {
    vec2 ns = edge_normal(sd); vec2 ts = edge_tangent(sd);
    vec2 nd = edge_normal(dd); vec2 td = edge_tangent(dd);
    float qn = dot(q, nd); float qt = dot(q, td);
    return (-qn) * ns + ((reversed ? -1.0 : 1.0) * qt) * ts;
}
vec2 source_to_destination(vec2 q, uint sd, uint dd, bool reversed) {
    vec2 ns = edge_normal(sd); vec2 ts = edge_tangent(sd);
    vec2 nd = edge_normal(dd); vec2 td = edge_tangent(dd);
    float qn = dot(q, ns); float qt = dot(q, ts);
    return (-qn) * nd + ((reversed ? -1.0 : 1.0) * qt) * td;
}
vec3 reconstruct(vec3 q, float h) {
    if (q.x <= dry_eps() || h <= dry_eps()) return vec3(0.0);
    return vec3(h, q.yz * (h / q.x));
}
vec3 normal_flux(vec3 q) {
    if (q.x <= dry_eps()) return vec3(0.0);
    float un = q.y / q.x;
    return vec3(q.y, q.y * un + 0.5 * grav() * q.x * q.x, q.z * un);
}
float normal_signal(vec3 q) {
    if (q.x <= dry_eps()) return 0.0;
    return abs(q.y / q.x) + sqrt(grav() * q.x);
}
vec3 rusanov(vec3 l, vec3 r) {
    float a = max(normal_signal(l), normal_signal(r));
    return 0.5 * (normal_flux(l) + normal_flux(r)) - 0.5 * a * (r - l);
}
vec3 interface_flux(vec4 c, vec4 f, out float hc, out float hf) {
    float zstar = max(c.w, f.w);
    hc = max(c.x + c.w - zstar, 0.0);
    hf = max(f.x + f.w - zstar, 0.0);
    return rusanov(reconstruct(c.xyz, hc), reconstruct(f.xyz, hf));
}

void main() {
    if (params.physics.w < 0.5 || control.iteration_active == 0u
            || pc.interface_index >= params.grid.z) return;
    uint lane = gl_GlobalInvocationID.x;
    uint r = tile_res();
    if (lane >= r) return;

    uvec4 a = interface_words[pc.interface_index * 2u];
    uvec4 b = interface_words[pc.interface_index * 2u + 1u];
    uint coarse_slot = a.x; uint fine_low_slot = a.y; uint fine_high_slot = a.z;
    uint coarse_direction = a.w; uint fine_direction = b.x; bool reversed = b.y != 0u;
    if (!live_slot(coarse_slot) || coarse_direction > 3u || fine_direction > 3u) return;
    bool low_live = live_slot(fine_low_slot); bool high_live = live_slot(fine_high_slot);
    if (!low_live && !high_live) return;

    int base_level = int(params.grid.w);
    int coarse_level = tile_metadata[coarse_slot].y;
    if (coarse_level < 0 || coarse_level >= base_level) return;
    if (low_live && tile_metadata[fine_low_slot].y != coarse_level + 1) return;
    if (high_live && tile_metadata[fine_high_slot].y != coarse_level + 1) return;

    uint coarse_lod = uint(max(base_level - coarse_level, 1));
    uint fine_lod = coarse_lod - 1u;
    float fine_dt = lod_dt(fine_lod);
    float coarse_dt = lod_dt(coarse_lod);
    if (fine_dt <= 0.0 && coarse_dt <= 0.0) return;

    float coarse_dx = max(params.physics.x, 1e-4) * exp2(float(coarse_lod));
    float fine_dx = coarse_dx * 0.5;
    float coarse_area = coarse_dx * coarse_dx;
    float fine_area = fine_dx * fine_dx;
    uint coarse_i = idx(coarse_slot, edge_cell(coarse_direction, lane));
    vec4 coarse_before = cells_in[coarse_i];
    if (any(isnan(coarse_before)) || any(isinf(coarse_before))) return;

    vec2 nc = edge_normal(coarse_direction); vec2 tc = edge_tangent(coarse_direction);
    float coarse_h = max(coarse_before.x, 0.0);
    float coarse_qn = dot(coarse_before.yz, nc); float coarse_qt = dot(coarse_before.yz, tc);
    vec4 coarse_nt = vec4(coarse_h, coarse_qn, coarse_qt, coarse_before.w);

    uint register_i = pc.interface_index * r + lane;
    vec3 accumulated = coarse_delta_nt[register_i].xyz;
    float represented_fraction = 0.0;

    if (fine_dt > 0.0) {
        for (uint sub = 0u; sub < 2u; ++sub) {
            uint source_global = lane * 2u + sub;
            uint destination_global = reversed ? (2u * r - 1u - source_global) : source_global;
            uint fine_slot = destination_global < r ? fine_low_slot : fine_high_slot;
            if (!live_slot(fine_slot)) continue;
            represented_fraction += 0.5;
            uint fine_k = destination_global % r;
            uint fi = idx(fine_slot, edge_cell(fine_direction, fine_k));
            vec4 fine_before = cells_in[fi]; vec4 fine_after = cells_out[fi];
            if (any(isnan(fine_before)) || any(isinf(fine_before))
                    || any(isnan(fine_after)) || any(isinf(fine_after))) continue;

            vec2 fine_q_source = destination_to_source(fine_before.yz,
                coarse_direction, fine_direction, reversed);
            vec4 fine_nt = vec4(max(fine_before.x, 0.0),
                dot(fine_q_source, nc), dot(fine_q_source, tc), fine_before.w);
            float hc; float hf;
            vec3 flux = interface_flux(coarse_nt, fine_nt, hc, hf);
            float segment_volume = flux.x * fine_dt * fine_dx;

            float limiter = 1.0;
            if (segment_volume < 0.0) {
                float fine_available = max(fine_after.x, 0.0) * fine_area;
                limiter = min(1.0, fine_available / max(-segment_volume, 1e-20));
            } else if (segment_volume > 0.0) {
                float coarse_reserved_h = accumulated.x;
                float coarse_available = max(coarse_h + coarse_reserved_h, 0.0) * coarse_area;
                limiter = min(1.0, coarse_available / max(segment_volume, 1e-20));
            }
            float actual_volume = segment_volume * limiter;

            float coarse_scale = fine_dt * fine_dx / coarse_area;
            vec3 coarse_delta = vec3(
                -flux.x * coarse_scale,
                (-flux.y + 0.5 * grav() * hc * hc) * coarse_scale,
                -flux.z * coarse_scale) * limiter;
            coarse_delta.x = -actual_volume / coarse_area;
            accumulated += coarse_delta;

            float fine_scale = fine_dt / fine_dx;
            vec2 desired_common = nc * ((flux.y - 0.5 * grav() * hf * hf) * fine_scale)
                + tc * (flux.z * fine_scale);
            vec2 desired_local = source_to_destination(desired_common,
                coarse_direction, fine_direction, reversed) * limiter;
            vec2 nf = edge_normal(fine_direction);
            float fine_h = max(fine_before.x, 0.0);
            float fine_qn = dot(fine_before.yz, nf);
            vec2 wall_local = vec2(0.0);
            if (fine_h > dry_eps())
                wall_local = nf * (-(fine_qn * fine_qn / fine_h) * fine_dt / fine_dx);

            fine_after.x += actual_volume / fine_area;
            fine_after.yz += desired_local - wall_local;
            if (fine_after.x >= 0.0 && fine_after.x <= dry_eps()) {
                fine_after.x = 0.0; fine_after.yz = vec2(0.0);
            }
            cells_out[fi] = fine_after;
        }
    } else {
        // Needed only to compute the represented coarse-wall fraction on a coarse
        // synchronization tick. Under the binary schedule coarse due implies fine
        // due, but keeping this path makes forced/recovery synchronization robust.
        uint first_global = lane * 2u;
        for (uint sub = 0u; sub < 2u; ++sub) {
            uint destination_global = reversed ? (2u * r - 1u - (first_global + sub)) : first_global + sub;
            uint fine_slot = destination_global < r ? fine_low_slot : fine_high_slot;
            if (live_slot(fine_slot)) represented_fraction += 0.5;
        }
    }

    if (coarse_dt > 0.0 && represented_fraction > 0.0) {
        vec4 coarse_after = cells_out[coarse_i];
        if (any(isnan(coarse_after)) || any(isinf(coarse_after))) return;
        if (coarse_h > dry_eps()) {
            float wall_qn_delta = -(coarse_qn * coarse_qn / coarse_h) * coarse_dt / coarse_dx;
            accumulated.y -= wall_qn_delta * represented_fraction;
        }
        coarse_after.x += accumulated.x;
        coarse_after.yz += nc * accumulated.y + tc * accumulated.z;
        if (coarse_after.x >= 0.0 && coarse_after.x <= dry_eps()) {
            coarse_after.x = 0.0; coarse_after.yz = vec2(0.0);
        }
        cells_out[coarse_i] = coarse_after;
        accumulated = vec3(0.0);
    }

    coarse_delta_nt[register_i] = vec4(accumulated, 0.0);
}

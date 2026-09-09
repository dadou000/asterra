#[compute]
#version 450

// Sparse shallow-water update with temporal HydroLOD subcycling.
// H0 advances on every fine-clock CFL tick; Hn advances only when its accumulated
// 2^n-tick interval is due. A GPU due-slot queue compacts only the occupied levels
// scheduled for this tick, and indirect dispatch makes gl_GlobalInvocationID.z an
// index into that queue rather than an atlas slot. Non-due slots launch no SWE work.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer StateIn { vec4 cells_in[]; };
layout(set = 0, binding = 1, std430) writeonly buffer StateOut { vec4 cells_out[]; };
layout(set = 0, binding = 2, std430) readonly buffer Sources { vec4 sources[]; };
layout(set = 0, binding = 3, std430) readonly buffer Occupancy { int occupied[]; };
layout(set = 0, binding = 4, std430) readonly buffer NeighborSlots { int neighbor_slots[]; };
layout(set = 0, binding = 5, std430) readonly buffer NeighborLinks { int neighbor_links[]; };
layout(set = 0, binding = 6, std430) readonly buffer Params {
    vec4 grid_dt;
    vec4 physics;
    vec4 schedule; // max substeps, H0 tile level, HydroLOD enabled, temporal max+1
} params;
layout(set = 0, binding = 7, std430) readonly buffer Control {
    uint pre_max_speed_bits; uint pre_max_depth_bits; uint pre_wet_count; uint pre_invalid_count;
    uint post_max_speed_bits; uint post_max_depth_bits; uint post_wet_count; uint post_invalid_count;
    float requested_dt; float remaining_dt; float current_dt; float advanced_dt;
    float min_cfl_dt; float last_cfl_dt; uint steps_taken; uint max_substeps;
    uint cfl_clamped; uint iteration_active; uint iter_max_speed_bits; uint iter_max_depth_bits;
    uint iter_wet_count; uint iter_invalid_count; uint reserved0; uint reserved1;
    uint iter_max_cfl_rate_bits; uint reserved2;
    uint temporal_enabled; uint temporal_max_lod; uint temporal_fine_tick; uint temporal_force_sync;
    float temporal_accum0; float temporal_accum1; float temporal_accum2; float temporal_accum3;
    float temporal_accum4; float temporal_accum5; float temporal_accum6; float temporal_accum7;
    float temporal_step0; float temporal_step1; float temporal_step2; float temporal_step3;
    float temporal_step4; float temporal_step5; float temporal_step6; float temporal_step7;
} control;
layout(set = 0, binding = 8, std430) readonly buffer AtmosphericSources {
    vec4 atmospheric_sources[];
};
layout(set = 0, binding = 9, std430) buffer ExternalFluxLedger {
    vec2 external_flux_m3[];
};
layout(set = 0, binding = 10, std430) readonly buffer TileMetadata {
    ivec4 tile_metadata[];
};
layout(set = 0, binding = 11, std430) readonly buffer DueSlots {
    uint due_slots[];
};

layout(push_constant, std430) uniform StepPush {
    uint step_index; uint pad0; uint pad1; uint pad2;
} pc;

const int DIR_WEST = 0;
const int DIR_EAST = 1;
const int DIR_SOUTH = 2;
const int DIR_NORTH = 3;
const int REVERSED_BIT = 1 << 2;

struct HydroInterface { vec3 flux; float h_left; float h_right; };

int tile_res() { return max(int(params.grid_dt.x + 0.5), 1); }
int capacity() { return max(int(params.grid_dt.y + 0.5), 1); }
float grav() { return max(params.physics.x, 1e-4); }
float dry_eps() { return max(params.physics.y, 1e-8); }
float manning_n() { return max(params.physics.z, 0.0); }

int physical_lod(int slot) {
    if (params.schedule.z < 0.5) return 0;
    int base_level = int(params.schedule.y + 0.5);
    return max(base_level - tile_metadata[slot].y, 0);
}

float scheduled_dt(int slot) {
    if (control.temporal_enabled == 0u) return max(control.current_dt, 0.0);
    int lod = clamp(physical_lod(slot), 0, int(min(control.temporal_max_lod, 7u)));
    if (lod == 0) return control.temporal_step0;
    if (lod == 1) return control.temporal_step1;
    if (lod == 2) return control.temporal_step2;
    if (lod == 3) return control.temporal_step3;
    if (lod == 4) return control.temporal_step4;
    if (lod == 5) return control.temporal_step5;
    if (lod == 6) return control.temporal_step6;
    return control.temporal_step7;
}

float slot_dx(int slot) {
    float base_dx = max(params.grid_dt.z, 1e-4);
    if (params.schedule.z < 0.5) return base_dx;
    return base_dx * exp2(float(physical_lod(slot)));
}

int cells_per_tile() { int r = tile_res(); return r * r; }
int idx(int slot, ivec2 p) { return slot * cells_per_tile() + p.y * tile_res() + p.x; }

vec2 edge_normal(int direction) {
    if (direction == DIR_WEST) return vec2(-1.0, 0.0);
    if (direction == DIR_EAST) return vec2(1.0, 0.0);
    if (direction == DIR_SOUTH) return vec2(0.0, -1.0);
    return vec2(0.0, 1.0);
}
vec2 edge_tangent(int direction) {
    return (direction == DIR_WEST || direction == DIR_EAST) ? vec2(0.0, 1.0) : vec2(1.0, 0.0);
}
vec2 momentum_to_source(vec2 q_dest, int source_direction,
        int destination_direction, bool reversed) {
    vec2 ns = edge_normal(source_direction); vec2 ts = edge_tangent(source_direction);
    vec2 nd = edge_normal(destination_direction); vec2 td = edge_tangent(destination_direction);
    float qn = dot(q_dest, nd); float qt = dot(q_dest, td);
    return (-qn) * ns + ((reversed ? -1.0 : 1.0) * qt) * ts;
}
ivec2 edge_cell(int direction, int k) {
    int r = tile_res();
    if (direction == DIR_WEST) return ivec2(0, k);
    if (direction == DIR_EAST) return ivec2(r - 1, k);
    if (direction == DIR_SOUTH) return ivec2(k, 0);
    return ivec2(k, r - 1);
}
vec4 reflected_neighbor(vec4 center, int source_direction) {
    vec4 result = center;
    if (source_direction == DIR_WEST || source_direction == DIR_EAST) result.y = -result.y;
    else result.z = -result.z;
    return result;
}
vec4 boundary_neighbor(int source_slot, int source_direction,
        int source_edge_index, vec4 center) {
    int ci = source_slot * 4 + source_direction;
    int destination_slot = neighbor_slots[ci];
    int link = neighbor_links[ci];
    if (destination_slot < 0 || destination_slot >= capacity() || link < 0
            || occupied[destination_slot] == 0)
        return reflected_neighbor(center, source_direction);
    int destination_direction = link & 3;
    bool reversed = (link & REVERSED_BIT) != 0;
    int k = reversed ? tile_res() - 1 - source_edge_index : source_edge_index;
    vec4 result = cells_in[idx(destination_slot, edge_cell(destination_direction, k))];
    result.yz = momentum_to_source(result.yz, source_direction,
        destination_direction, reversed);
    return result;
}
vec4 neighbor_at(int slot, ivec2 p, int direction, int edge_index, vec4 center) {
    int r = tile_res();
    if (p.x >= 0 && p.y >= 0 && p.x < r && p.y < r) return cells_in[idx(slot, p)];
    return boundary_neighbor(slot, direction, edge_index, center);
}

vec3 reconstruct(vec3 q, float reconstructed_h) {
    if (q.x <= dry_eps() || reconstructed_h <= dry_eps()) return vec3(0.0);
    float scale = reconstructed_h / q.x;
    return vec3(reconstructed_h, q.yz * scale);
}
vec3 flux_x(vec3 q) {
    if (q.x <= dry_eps()) return vec3(0.0);
    float u = q.y / q.x;
    return vec3(q.y, q.y * u + 0.5 * grav() * q.x * q.x, q.z * u);
}
vec3 flux_y(vec3 q) {
    if (q.x <= dry_eps()) return vec3(0.0);
    float v = q.z / q.x;
    return vec3(q.z, q.y * v, q.z * v + 0.5 * grav() * q.x * q.x);
}
float signal_x(vec3 q) {
    if (q.x <= dry_eps()) return 0.0;
    return abs(q.y / q.x) + sqrt(grav() * q.x);
}
float signal_y(vec3 q) {
    if (q.x <= dry_eps()) return 0.0;
    return abs(q.z / q.x) + sqrt(grav() * q.x);
}
vec3 rusanov_x(vec3 l, vec3 r) {
    float a = max(signal_x(l), signal_x(r));
    return 0.5 * (flux_x(l) + flux_x(r)) - 0.5 * a * (r - l);
}
vec3 rusanov_y(vec3 l, vec3 r) {
    float a = max(signal_y(l), signal_y(r));
    return 0.5 * (flux_y(l) + flux_y(r)) - 0.5 * a * (r - l);
}
HydroInterface interface_x(vec3 l, float zl, vec3 r, float zr) {
    float zstar = max(zl, zr);
    float hl = max(l.x + zl - zstar, 0.0); float hr = max(r.x + zr - zstar, 0.0);
    HydroInterface result; result.flux = rusanov_x(reconstruct(l, hl), reconstruct(r, hr));
    result.h_left = hl; result.h_right = hr; return result;
}
HydroInterface interface_y(vec3 l, float zl, vec3 r, float zr) {
    float zstar = max(zl, zr);
    float hl = max(l.x + zl - zstar, 0.0); float hr = max(r.x + zr - zstar, 0.0);
    HydroInterface result; result.flux = rusanov_y(reconstruct(l, hl), reconstruct(r, hr));
    result.h_left = hl; result.h_right = hr; return result;
}
vec3 apply_friction(vec3 q, float step_dt) {
    float n = manning_n();
    if (q.x <= 1e-3 || n <= 0.0) return q;
    vec2 velocity = q.yz / q.x; float speed = length(velocity);
    if (speed <= 1e-8) return q;
    float denom = 1.0 + step_dt * grav() * n * n * speed / pow(q.x, 4.0 / 3.0);
    return vec3(q.x, q.yz / denom);
}
vec3 apply_sources(vec3 q, vec4 source, float step_dt) {
    float add_h = max(source.x, 0.0) * step_dt;
    float remove_h = max(source.y, 0.0) * step_dt;
    q.x += add_h; q.yz += source.zw * step_dt;
    if (remove_h > 0.0 && q.x > 0.0) {
        float kept_h = max(q.x - remove_h, 0.0);
        float keep_fraction = kept_h / max(q.x, 1e-12);
        q.yz *= keep_fraction; q.x = kept_h;
    }
    return q;
}

void main() {
    if (pc.step_index >= control.steps_taken || control.iteration_active == 0u
            || control.current_dt <= 0.0) return;

    ivec3 gid = ivec3(gl_GlobalInvocationID.xyz);
    int r = tile_res();
    if (gid.x >= r || gid.y >= r) return;

    uint queue_index = uint(gid.z);
    if (queue_index >= uint(capacity())) return;
    int slot = int(due_slots[queue_index]);
    if (slot < 0 || slot >= capacity() || occupied[slot] == 0) return;

    // The queue already guarantees this slot is due. Keep the dt guard as a
    // fail-closed cross-check against a stale/corrupt indirect queue.
    float step_dt = scheduled_dt(slot);
    if (step_dt <= 0.0) return;

    ivec2 p = gid.xy; int i = idx(slot, p);
    vec4 packed_c = cells_in[i]; vec3 c = packed_c.xyz; float zc = packed_c.w;
    vec4 packed_w = neighbor_at(slot, p + ivec2(-1,0), DIR_WEST, p.y, packed_c);
    vec4 packed_e = neighbor_at(slot, p + ivec2(1,0), DIR_EAST, p.y, packed_c);
    vec4 packed_s = neighbor_at(slot, p + ivec2(0,-1), DIR_SOUTH, p.x, packed_c);
    vec4 packed_n = neighbor_at(slot, p + ivec2(0,1), DIR_NORTH, p.x, packed_c);
    HydroInterface fw = interface_x(packed_w.xyz, packed_w.w, c, zc);
    HydroInterface fe = interface_x(c, zc, packed_e.xyz, packed_e.w);
    HydroInterface fs = interface_y(packed_s.xyz, packed_s.w, c, zc);
    HydroInterface fn = interface_y(c, zc, packed_n.xyz, packed_n.w);

    float local_dx = slot_dx(slot); float scale = step_dt / local_dx;
    vec3 updated = c - (fe.flux - fw.flux) * scale - (fn.flux - fs.flux) * scale;
    updated.y += 0.5 * grav() * scale * (fe.h_left * fe.h_left - fw.h_right * fw.h_right);
    updated.z += 0.5 * grav() * scale * (fn.h_left * fn.h_left - fs.h_right * fs.h_right);

    vec4 source = sources[i] + atmospheric_sources[i];
    float add_h = max(source.x, 0.0) * step_dt;
    float requested_remove_h = max(source.y, 0.0) * step_dt;
    float available_after_add = max(updated.x + add_h, 0.0);
    float actual_remove_h = min(requested_remove_h, available_after_add);
    float cell_area_m2 = local_dx * local_dx;
    external_flux_m3[i] += vec2(add_h, actual_remove_h) * cell_area_m2;
    updated = apply_sources(updated, source, step_dt);

    if (updated.x <= dry_eps() || any(isnan(updated)) || any(isinf(updated))) updated = vec3(0.0);
    else updated = apply_friction(updated, step_dt);
    cells_out[i] = vec4(updated, zc);
}

#[compute]
#version 450

// Same-level sparse-tile shallow-water update.
//
// State layout is one contiguous tile after another, each cell storing:
//   h, hu, hv, bed elevation
// where hu/hv are momentum in the tile's local +u/+v tangent frame.
//
// Interior interfaces read the same tile. A resident boundary reads the exact
// neighboring tile edge described by SparseHydroConnectivityGPU. Across cube
// seams the edge index may reverse and the neighbor momentum is rotated into the
// source tile frame before the Riemann problem is solved. A nonresident boundary
// is temporarily reflective; the separate frontier queue is responsible for
// allocating/waking the destination tile.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer StateIn {
    vec4 cells_in[];
};
layout(set = 0, binding = 1, std430) writeonly buffer StateOut {
    vec4 cells_out[];
};
layout(set = 0, binding = 2, std430) readonly buffer Sources {
    vec4 sources[]; // rain m/s, infiltration m/s, reserved, reserved
};
layout(set = 0, binding = 3, std430) readonly buffer Occupancy {
    int occupied[];
};
layout(set = 0, binding = 4, std430) readonly buffer NeighborSlots {
    int neighbor_slots[]; // slot*4 + W/E/S/N -> resident slot or -1
};
layout(set = 0, binding = 5, std430) readonly buffer NeighborLinks {
    int neighbor_links[]; // destination edge[0..3] | reversed<<2, or -1
};
layout(set = 0, binding = 6, std430) readonly buffer Params {
    vec4 grid_dt; // tile_resolution, capacity, cell_size_m, dt_s
    vec4 physics; // gravity, dry_eps, Manning n, reserved
} params;

const int DIR_WEST = 0;
const int DIR_EAST = 1;
const int DIR_SOUTH = 2;
const int DIR_NORTH = 3;
const int REVERSED_BIT = 1 << 2;

struct HydroInterface {
    vec3 flux;
    float h_left;
    float h_right;
};

int tile_res() { return max(int(params.grid_dt.x + 0.5), 1); }
int capacity() { return max(int(params.grid_dt.y + 0.5), 1); }
float dx() { return max(params.grid_dt.z, 1e-4); }
float dt() { return max(params.grid_dt.w, 0.0); }
float grav() { return max(params.physics.x, 1e-4); }
float dry_eps() { return max(params.physics.y, 1e-8); }
float manning_n() { return max(params.physics.z, 0.0); }

int cells_per_tile() {
    int r = tile_res();
    return r * r;
}

int idx(int slot, ivec2 p) {
    return slot * cells_per_tile() + p.y * tile_res() + p.x;
}

vec2 edge_normal(int direction) {
    if (direction == DIR_WEST) return vec2(-1.0, 0.0);
    if (direction == DIR_EAST) return vec2( 1.0, 0.0);
    if (direction == DIR_SOUTH) return vec2(0.0, -1.0);
    return vec2(0.0, 1.0); // north
}

vec2 edge_tangent(int direction) {
    // Increasing source edge parameter: +v on W/E, +u on S/N.
    if (direction == DIR_WEST || direction == DIR_EAST)
        return vec2(0.0, 1.0);
    return vec2(1.0, 0.0);
}

// Convert destination-local momentum into source-local momentum.
// Destination outward normal is physically opposite source outward normal.
// Destination increasing edge tangent agrees with source when orientation=+1,
// and is physically reversed when orientation=-1.
vec2 momentum_to_source(vec2 q_dest, int source_direction,
        int destination_direction, bool reversed) {
    vec2 ns = edge_normal(source_direction);
    vec2 ts = edge_tangent(source_direction);
    vec2 nd = edge_normal(destination_direction);
    vec2 td = edge_tangent(destination_direction);
    float qn = dot(q_dest, nd);
    float qt = dot(q_dest, td);
    float orientation = reversed ? -1.0 : 1.0;
    return (-qn) * ns + (orientation * qt) * ts;
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
    if (source_direction == DIR_WEST || source_direction == DIR_EAST)
        result.y = -result.y;
    else
        result.z = -result.z;
    return result;
}

vec4 boundary_neighbor(int source_slot, int source_direction,
        int source_edge_index, vec4 center) {
    int ci = source_slot * 4 + source_direction;
    int destination_slot = neighbor_slots[ci];
    int link = neighbor_links[ci];
    if (destination_slot < 0 || destination_slot >= capacity()
            || link < 0 || occupied[destination_slot] == 0) {
        return reflected_neighbor(center, source_direction);
    }

    int destination_direction = link & 3;
    bool reversed = (link & REVERSED_BIT) != 0;
    int k = reversed ? (tile_res() - 1 - source_edge_index) : source_edge_index;
    vec4 result = cells_in[idx(destination_slot, edge_cell(destination_direction, k))];
    result.yz = momentum_to_source(result.yz, source_direction,
        destination_direction, reversed);
    return result;
}

vec4 neighbor_at(int slot, ivec2 p, int direction, int edge_index, vec4 center) {
    int r = tile_res();
    if (p.x >= 0 && p.y >= 0 && p.x < r && p.y < r)
        return cells_in[idx(slot, p)];
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
    float hl = max(l.x + zl - zstar, 0.0);
    float hr = max(r.x + zr - zstar, 0.0);
    HydroInterface result;
    result.flux = rusanov_x(reconstruct(l, hl), reconstruct(r, hr));
    result.h_left = hl;
    result.h_right = hr;
    return result;
}

HydroInterface interface_y(vec3 lower, float zl, vec3 upper, float zr) {
    float zstar = max(zl, zr);
    float hl = max(lower.x + zl - zstar, 0.0);
    float hr = max(upper.x + zr - zstar, 0.0);
    HydroInterface result;
    result.flux = rusanov_y(reconstruct(lower, hl), reconstruct(upper, hr));
    result.h_left = hl;
    result.h_right = hr;
    return result;
}

vec3 apply_friction(vec3 q, float step_dt) {
    float n = manning_n();
    if (q.x <= 1e-3 || n <= 0.0) return q;
    vec2 velocity = q.yz / q.x;
    float speed = length(velocity);
    if (speed <= 1e-8) return q;
    float denom = 1.0 + step_dt * grav() * n * n * speed / pow(q.x, 4.0 / 3.0);
    return vec3(q.x, q.yz / denom);
}

void main() {
    ivec3 gid = ivec3(gl_GlobalInvocationID.xyz);
    int slot = gid.z;
    int r = tile_res();
    if (slot >= capacity() || gid.x >= r || gid.y >= r) return;

    ivec2 p = gid.xy;
    int i = idx(slot, p);

    // Keep unoccupied scratch deterministic. No state from a recycled slot is
    // allowed to become authoritative merely because a broad z-dispatch touched it.
    if (occupied[slot] == 0) {
        cells_out[i] = vec4(0.0);
        return;
    }

    vec4 packed_c = cells_in[i];
    vec3 c = packed_c.xyz;
    float zc = packed_c.w;

    vec4 packed_w = neighbor_at(slot, p + ivec2(-1, 0), DIR_WEST, p.y, packed_c);
    vec4 packed_e = neighbor_at(slot, p + ivec2( 1, 0), DIR_EAST, p.y, packed_c);
    vec4 packed_s = neighbor_at(slot, p + ivec2( 0,-1), DIR_SOUTH, p.x, packed_c);
    vec4 packed_n = neighbor_at(slot, p + ivec2( 0, 1), DIR_NORTH, p.x, packed_c);

    HydroInterface fw = interface_x(packed_w.xyz, packed_w.w, c, zc);
    HydroInterface fe = interface_x(c, zc, packed_e.xyz, packed_e.w);
    HydroInterface fs = interface_y(packed_s.xyz, packed_s.w, c, zc);
    HydroInterface fn = interface_y(c, zc, packed_n.xyz, packed_n.w);

    float scale = dt() / dx();
    vec3 updated = c
        - (fe.flux - fw.flux) * scale
        - (fn.flux - fs.flux) * scale;

    updated.y += 0.5 * grav() * scale
        * (fe.h_left * fe.h_left - fw.h_right * fw.h_right);
    updated.z += 0.5 * grav() * scale
        * (fn.h_left * fn.h_left - fs.h_right * fs.h_right);

    vec4 source = sources[i];
    updated.x = max(updated.x
        + (max(source.x, 0.0) - max(source.y, 0.0)) * dt(), 0.0);

    if (updated.x <= dry_eps()
            || any(isnan(updated)) || any(isinf(updated))) {
        updated = vec3(0.0);
    } else {
        updated = apply_friction(updated, dt());
    }

    cells_out[i] = vec4(updated, zc);
}

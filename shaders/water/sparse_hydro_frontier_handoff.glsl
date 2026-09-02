#[compute]
#version 450

// Conservative same-level pre-wetting handoff for a newly reserved sparse tile.
//
// Preconditions:
// - source slot is active/occupied;
// - destination slot is reserved in CPU policy but NOT yet GPU-occupied;
// - destination A state was staged with dry h/hu/hv and its own terrain bed;
// - connectivity has NOT yet been opened between source/destination.
//
// One invocation owns one source/destination edge-cell pair. It removes a small
// CFL-like Rusanov-predicted water parcel from the source and adds exactly that
// depth to the destination. The parcel's existing momentum is removed from the
// source and rotated into the destination local face frame before being added.
// Thus same-level cell area, water volume, and pre-existing physical momentum are
// conserved by the handoff itself. Hydrostatic pressure dynamics are left to the
// normal connected SWE step after activation.
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) buffer State {
    vec4 cells[]; // h, hu, hv, bed
};
layout(set = 0, binding = 1, std430) readonly buffer Occupancy {
    int occupied[];
};
layout(set = 0, binding = 2, std430) readonly buffer Params {
    uvec4 slots_dirs; // source_slot, destination_slot, source_dir, destination_dir
    uvec4 topology;   // reversed, tile_res, capacity, reserved
    vec4 physical;   // dx, seed_dt, max_source_fraction, gravity
} params;

const uint DIR_WEST = 0u;
const uint DIR_EAST = 1u;
const uint DIR_SOUTH = 2u;
const uint DIR_NORTH = 3u;

uint cells_per_tile() {
    uint r = max(params.topology.y, 1u);
    return r * r;
}

uint idx(uint slot, uvec2 p) {
    uint r = max(params.topology.y, 1u);
    return slot * cells_per_tile() + p.y * r + p.x;
}

vec2 edge_normal(uint direction) {
    if (direction == DIR_WEST) return vec2(-1.0, 0.0);
    if (direction == DIR_EAST) return vec2(1.0, 0.0);
    if (direction == DIR_SOUTH) return vec2(0.0, -1.0);
    return vec2(0.0, 1.0);
}

vec2 edge_tangent(uint direction) {
    return (direction == DIR_WEST || direction == DIR_EAST)
        ? vec2(0.0, 1.0) : vec2(1.0, 0.0);
}

uvec2 edge_cell(uint direction, uint k) {
    uint r = max(params.topology.y, 1u);
    if (direction == DIR_WEST) return uvec2(0u, k);
    if (direction == DIR_EAST) return uvec2(r - 1u, k);
    if (direction == DIR_SOUTH) return uvec2(k, 0u);
    return uvec2(k, r - 1u);
}

// Source-local momentum -> destination-local representation of the same physical
// tangent vector. Destination outward normal is physically -source outward normal;
// destination increasing tangent is +/- source tangent according to seam ordering.
vec2 momentum_to_destination(vec2 q_source, uint source_direction,
        uint destination_direction, bool reversed) {
    vec2 ns = edge_normal(source_direction);
    vec2 ts = edge_tangent(source_direction);
    vec2 nd = edge_normal(destination_direction);
    vec2 td = edge_tangent(destination_direction);
    float qn_source = dot(q_source, ns);
    float qt_source = dot(q_source, ts);
    float orientation = reversed ? -1.0 : 1.0;
    float qn_destination = -qn_source;
    float qt_destination = orientation * qt_source;
    return qn_destination * nd + qt_destination * td;
}

float predicted_dry_flux_per_width(float h, float outward_momentum, float gravity) {
    if (h <= 1e-8) return 0.0;
    float velocity = outward_momentum / h;
    float a = abs(velocity) + sqrt(max(gravity, 1e-4) * h);
    return max(0.5 * (outward_momentum + a * h), 0.0);
}

void main() {
    uint k = gl_GlobalInvocationID.x;
    uint r = max(params.topology.y, 1u);
    uint capacity = max(params.topology.z, 1u);
    if (k >= r) return;

    uint source_slot = params.slots_dirs.x;
    uint destination_slot = params.slots_dirs.y;
    uint source_direction = params.slots_dirs.z;
    uint destination_direction = params.slots_dirs.w;
    if (source_slot >= capacity || destination_slot >= capacity
            || source_slot == destination_slot || source_direction > 3u
            || destination_direction > 3u) return;
    if (occupied[source_slot] == 0) return;
    // A reserved destination must still be unpublished. Refuse to mutate a live
    // resident tile if policy accidentally routes an already-active neighbor here.
    if (occupied[destination_slot] != 0) return;

    bool reversed = params.topology.x != 0u;
    uint destination_k = reversed ? (r - 1u - k) : k;
    uint source_i = idx(source_slot, edge_cell(source_direction, k));
    uint destination_i = idx(destination_slot,
        edge_cell(destination_direction, destination_k));

    vec4 source = cells[source_i];
    vec4 destination = cells[destination_i];
    float h = max(source.x, 0.0);
    if (h <= 1e-8 || any(isnan(source)) || any(isinf(source))
            || any(isnan(destination)) || any(isinf(destination))) return;

    // Hydrostatic reconstruction against the destination bed prevents pre-wetting
    // over a barrier higher than the source free surface even if policy mistakenly
    // marked that boundary reachable.
    float zstar = max(source.w, destination.w);
    float reconstructed_h = max(source.w + h - zstar, 0.0);
    if (reconstructed_h <= 1e-8) return;

    vec2 ns = edge_normal(source_direction);
    float qn = dot(source.yz, ns);
    float momentum_scale = reconstructed_h / h;
    float predicted = predicted_dry_flux_per_width(
        reconstructed_h, qn * momentum_scale, max(params.physical.w, 1e-4));

    float dx = max(params.physical.x, 1e-4);
    float seed_dt = max(params.physical.y, 0.0);
    float max_fraction = clamp(params.physical.z, 0.0, 0.5);
    float dh = predicted * seed_dt / dx;
    dh = min(dh, h * max_fraction);
    dh = min(dh, reconstructed_h);
    if (dh <= 1e-9) return;

    float parcel_fraction = clamp(dh / h, 0.0, 1.0);
    vec2 parcel_q_source = source.yz * parcel_fraction;
    vec2 parcel_q_destination = momentum_to_destination(parcel_q_source,
        source_direction, destination_direction, reversed);

    source.x = max(h - dh, 0.0);
    source.yz -= parcel_q_source;
    if (source.x <= 1e-8) source.yz = vec2(0.0);

    destination.x = max(destination.x, 0.0) + dh;
    destination.yz += parcel_q_destination;

    cells[source_i] = source;
    cells[destination_i] = destination;
}

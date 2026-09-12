#[compute]
#version 450

// CFL/health reduction for temporal HydroLOD subcycling.
// When temporal subcycling is enabled, the reduced fine-clock rate is
// (|u|+c)/dx_local * 2^lod. Therefore a level-L update over 2^L accumulated H0
// ticks cannot exceed its own local CFL bound.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer State { vec4 cells[]; };
layout(set = 0, binding = 1, std430) readonly buffer Occupancy { int occupied[]; };
layout(set = 0, binding = 2, std430) buffer Control {
    uint pre_max_speed_bits; uint pre_max_depth_bits; uint pre_wet_count; uint pre_invalid_count;
    uint post_max_speed_bits; uint post_max_depth_bits; uint post_wet_count; uint post_invalid_count;
    float requested_dt; float remaining_dt; float current_dt; float advanced_dt;
    float min_cfl_dt; float last_cfl_dt; uint steps_taken; uint max_substeps;
    uint cfl_clamped; uint iteration_active; uint iter_max_speed_bits; uint iter_max_depth_bits;
    uint iter_wet_count; uint iter_invalid_count; uint reserved0; uint reserved1;
    uint iter_max_cfl_rate_bits; uint reserved2;
} control;
layout(set = 0, binding = 3, std430) readonly buffer Params {
    vec4 grid_dt;
    vec4 physics;
    vec4 schedule; // max substeps, H0 level, HydroLOD enabled, temporal max+1
} params;
layout(set = 0, binding = 4, std430) readonly buffer TileMetadata { ivec4 tile_metadata[]; };

layout(push_constant, std430) uniform ReducePush { uint mode; uint pad0; uint pad1; uint pad2; } pc;

shared uint group_max_speed_bits;
shared uint group_max_depth_bits;
shared uint group_max_cfl_rate_bits;
shared uint group_wet_count;
shared uint group_invalid_count;

int tile_res() { return max(int(params.grid_dt.x + 0.5), 1); }
int capacity() { return max(int(params.grid_dt.y + 0.5), 1); }
int cells_per_tile() { int r = tile_res(); return r * r; }
int physical_lod(int slot) {
    if (params.schedule.z < 0.5) return 0;
    int base_level = int(params.schedule.y + 0.5);
    return max(base_level - tile_metadata[slot].y, 0);
}
float slot_dx(int slot) {
    float base_dx = max(params.grid_dt.z, 1e-4);
    return base_dx * exp2(float(physical_lod(slot)));
}
int temporal_max_lod() {
    int encoded = int(params.schedule.w + 0.5);
    return encoded <= 0 ? -1 : encoded - 1;
}
bool invalid_state(vec4 s) {
    return any(isnan(s)) || any(isinf(s)) || s.x < -max(params.physics.y, 1e-8);
}

void main() {
    if (gl_LocalInvocationIndex == 0u) {
        group_max_speed_bits = 0u; group_max_depth_bits = 0u;
        group_max_cfl_rate_bits = 0u; group_wet_count = 0u; group_invalid_count = 0u;
    }
    barrier();

    uvec3 gid = gl_GlobalInvocationID.xyz;
    int r = tile_res(); int slot = int(gid.z);
    bool in_bounds = slot < capacity() && int(gid.x) < r && int(gid.y) < r;
    bool live = in_bounds && occupied[slot] != 0;
    if (live) {
        int i = slot * cells_per_tile() + int(gid.y) * r + int(gid.x);
        vec4 s = cells[i]; bool bad = invalid_state(s);
        float depth = bad ? 0.0 : max(s.x, 0.0);
        bool wet = depth > max(params.physics.y, 1e-8);
        float speed = 0.0; float cfl_rate = 0.0;
        if (wet) {
            vec2 velocity = s.yz / max(depth, 1e-8);
            float c = sqrt(max(params.physics.x, 1e-4) * depth);
            speed = max(abs(velocity.x) + c, abs(velocity.y) + c);
            cfl_rate = speed / slot_dx(slot);
            int max_lod = temporal_max_lod();
            if (max_lod >= 0) {
                int lod = clamp(physical_lod(slot), 0, max_lod);
                cfl_rate *= exp2(float(lod));
            }
            if (isnan(speed) || isinf(speed) || isnan(cfl_rate) || isinf(cfl_rate)) {
                speed = 0.0; cfl_rate = 0.0; bad = true;
            }
        }
        atomicMax(group_max_speed_bits, floatBitsToUint(max(speed, 0.0)));
        atomicMax(group_max_depth_bits, floatBitsToUint(max(depth, 0.0)));
        atomicMax(group_max_cfl_rate_bits, floatBitsToUint(max(cfl_rate, 0.0)));
        if (wet) atomicAdd(group_wet_count, 1u);
        if (bad) atomicAdd(group_invalid_count, 1u);
    }

    barrier();
    if (gl_LocalInvocationIndex != 0u) return;
    if (pc.mode == 0u) {
        atomicMax(control.iter_max_speed_bits, group_max_speed_bits);
        atomicMax(control.iter_max_depth_bits, group_max_depth_bits);
        atomicMax(control.iter_max_cfl_rate_bits, group_max_cfl_rate_bits);
        atomicAdd(control.iter_wet_count, group_wet_count);
        atomicAdd(control.iter_invalid_count, group_invalid_count);
    } else {
        atomicMax(control.post_max_speed_bits, group_max_speed_bits);
        atomicMax(control.post_max_depth_bits, group_max_depth_bits);
        atomicAdd(control.post_wet_count, group_wet_count);
        atomicAdd(control.post_invalid_count, group_invalid_count);
    }
}

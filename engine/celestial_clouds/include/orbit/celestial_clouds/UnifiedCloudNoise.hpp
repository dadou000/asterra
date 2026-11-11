#pragma once

#include <orbit/core/Types.hpp>

#include <vector>

namespace orbit::celestial_clouds::unified
{
// System B owns this immutable noise source, its sampling and its GPU buffer.
// A's baked noise, shader source, GPU resources and cache remain untouched.
inline constexpr u32 kUnifiedNoiseSide = 32U;
inline constexpr u32 kUnifiedNoiseChannels = 4U;

// Four tileable, normalized channels: broad structure, large cellular,
// intermediate cellular, fine cellular. CPU-baked once per B renderer.
// Every channel has identical indexing to a flat 32^3 float4 GPU buffer.
[[nodiscard]] std::vector<f32> BuildUnifiedCloudNoise();
} // namespace orbit::celestial_clouds::unified

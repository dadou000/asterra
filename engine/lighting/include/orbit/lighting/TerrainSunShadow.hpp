#pragma once

#include <orbit/math/Vector.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/terrain/TerrainSource.hpp>

#include <memory>
#include <stop_token>
#include <vector>

namespace orbit::lighting
{
// Disposable, body-fixed heightfield derived from the canonical TerrainSource.
// No camera, projection, render LOD or sun direction participates in its key.
struct TerrainSunShadowField
{
    u32 width{0U};
    u32 height{0U};
    f64 radiusMeters{0.0};
    f64 minimumElevationMeters{0.0};
    f64 maximumElevationMeters{0.0};
    f64 spacingMeters{0.0};
    u64 sourceRevision{0U};
    std::vector<f32> elevations;
};

[[nodiscard]] TerrainSunShadowField BuildTerrainSunShadowField(
    const terrain::TerrainSource& source, f64 radiusMeters,
    u32 width = 1024U, u32 height = 512U, std::stop_token cancellation = {});

// CPU reference for the shared GPU traversal, including longitude wrapping.
[[nodiscard]] f64 TerrainSunVisibility(const TerrainSunShadowField& field,
    math::Double3 position, math::Double3 sun, f64 sunAngularRadiusRadians = 0.0);

// Allocate a private, unpublished GPU candidate. Upload samples in bounded
// chunks on successive frames; only publish the buffer once fully populated.
// The caller must not bind the incomplete buffer into a render graph.
[[nodiscard]] std::shared_ptr<rhi::Buffer> AllocateTerrainSunShadowFieldBuffer(
    rhi::Device& device, const TerrainSunShadowField& field);
[[nodiscard]] std::size_t UploadTerrainSunShadowFieldChunk(
    rhi::Buffer& buffer, const TerrainSunShadowField& field,
    std::size_t firstSample, std::size_t maximumSamples);

[[nodiscard]] std::shared_ptr<rhi::Buffer> UploadTerrainSunShadowField(
    rhi::Device& device, const TerrainSunShadowField& field);
[[nodiscard]] std::unique_ptr<rhi::Buffer> CreateEmptyTerrainSunShadowBuffer(
    rhi::Device& device);
} // namespace orbit::lighting

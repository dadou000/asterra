#pragma once

#include <orbit/content/ContentService.hpp>
#include <orbit/content/RuntimeTexture.hpp>
#include <orbit/lighting/EmissiveHierarchy.hpp>
#include <orbit/lighting/RuntimeMaterialEmission.hpp>

#include <vector>

namespace orbit::lighting
{
enum class EmissiveTextureTransfer : u8
{
    Srgb,
    Linear
};

struct RuntimeEmissiveSurfaceGeometry
{
    frames::FrameId frame{};
    universe::BodyId body{};
    u64 stableId{0U};

    math::Double3 originInFrameMeters{};
    math::Double3 axisUInFrameMeters{1.0, 0.0, 0.0};
    math::Double3 axisVInFrameMeters{0.0, 1.0, 0.0};
};

struct RuntimeEmissiveSurface
{
    RuntimeEmissiveSurfaceGeometry geometry{};
    u32 width{0U};
    u32 height{0U};
    std::vector<math::Float3> giRadiance;

    [[nodiscard]] EmissiveSurfaceGrid Grid() const noexcept;
};

[[nodiscard]] RuntimeEmissiveSurface
BuildRuntimeEmissiveSurface(
    const RuntimeEmissiveSurfaceGeometry& geometry,
    const EvaluatedMaterialEmission& emission,
    const content::RuntimeTexture* emissiveTexture = nullptr,
    EmissiveTextureTransfer transfer = EmissiveTextureTransfer::Srgb);

[[nodiscard]] RuntimeEmissiveSurface
BuildRuntimeEmissiveSurface(
    content::ContentService& content,
    content::AssetId materialAsset,
    const RuntimeEmissiveSurfaceGeometry& geometry,
    EmissiveTextureTransfer transfer = EmissiveTextureTransfer::Srgb);
} // namespace orbit::lighting

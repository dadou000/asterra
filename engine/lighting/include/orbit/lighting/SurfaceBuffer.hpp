#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/lighting/SurfaceData.hpp>

#include <algorithm>
#include <cmath>

namespace orbit::lighting
{
// V0.0.7 M02 compact deferred surface-buffer contract.
//
// MRT 0 is transitional preview SceneColor and is NOT part of SurfaceData.
//
// MRT 1: SurfaceBaseRoughness  = baseColor.rgb, roughness
// MRT 2: SurfaceNormalMetallic = shadingNormal.xyz, metallic
// MRT 3: SurfaceEmissionClass  = emission.rgb, packed metadata
//
// Depth remains the authoritative geometric depth. Camera-relative position is
// reconstructed later from depth + LightingView (M04), avoiding another FP16
// position attachment.
//
// Metadata is encoded as class + representation/16. Both enums are deliberately
// kept < 16 for this V0.0.7 representation; RGBA16F represents the fractional
// sixteenth exactly for these small values.
inline constexpr f32 kSurfaceRepresentationDivisor = 16.0F;

[[nodiscard]] constexpr f32 EncodeSurfaceMetadata(
    const SurfaceClass surfaceClass,
    const SurfaceRepresentation representation) noexcept
{
    return
        static_cast<f32>(surfaceClass) +
        static_cast<f32>(representation) /
            kSurfaceRepresentationDivisor;
}

struct DecodedSurfaceMetadata
{
    SurfaceClass surfaceClass{SurfaceClass::Unknown};
    SurfaceRepresentation representation{
        SurfaceRepresentation::Unknown};
};

[[nodiscard]] inline DecodedSurfaceMetadata DecodeSurfaceMetadata(
    const f32 packed) noexcept
{
    if (!std::isfinite(packed) || packed < 0.0F)
    {
        return {};
    }

    const f32 classValue = std::floor(packed);
    const f32 representationValue =
        std::round(
            (packed - classValue) *
            kSurfaceRepresentationDivisor);

    const u32 classIndex =
        static_cast<u32>(
            std::clamp(classValue, 0.0F, 15.0F));
    const u32 representationIndex =
        static_cast<u32>(
            std::clamp(
                representationValue,
                0.0F,
                15.0F));

    return {
        .surfaceClass =
            static_cast<SurfaceClass>(classIndex),
        .representation =
            static_cast<SurfaceRepresentation>(
                representationIndex)
    };
}
} // namespace orbit::lighting

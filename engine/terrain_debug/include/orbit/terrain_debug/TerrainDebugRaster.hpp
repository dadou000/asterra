#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/terrain_debug/TerrainDebugField.hpp>

#include <span>
#include <vector>

namespace orbit::terrain_debug
{
struct TerrainDebugVector2
{
    f32 x{0.0F};
    f32 y{0.0F};
};

struct TerrainDebugRasterRange
{
    f32 minimum{0.0F};
    f32 maximum{1.0F};

    // When enabled, finite source values define the display range. Signed
    // scalar fields use a symmetric range around zero.
    bool automatic{true};
};

// A non-owning typed page-field binding. Exactly one source span is consumed,
// selected by Descriptor(field).valueClass. The binding is presentation-only;
// physical terrain authority remains in the source system that owns the data.
struct TerrainDebugRasterView
{
    TerrainDebugField field{TerrainDebugField::Uplift};
    u32 width{0};
    u32 height{0};
    TerrainDebugRasterRange range{};

    std::span<const f32> scalar;
    std::span<const TerrainDebugVector2> vector;
    std::span<const u32> category;
    std::span<const u8> boolean;
    std::span<const u64> revision;
    std::span<const u8> lod;
};

[[nodiscard]] u64 TerrainDebugTexelCount(
    const TerrainDebugRasterView& view) noexcept;

// Composes tightly packed RGBA8 texels suitable for upload through the RHI
// CopyBufferToTexture path. Non-finite numerical samples are deliberately
// rendered magenta so broken process state is visually unmistakable.
[[nodiscard]] std::vector<u8> ComposeTerrainDebugRgba8(
    const TerrainDebugRasterView& view);
} // namespace orbit::terrain_debug

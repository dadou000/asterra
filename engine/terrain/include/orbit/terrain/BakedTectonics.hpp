#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>

#include <array>
#include <cstddef>
#include <vector>

namespace orbit::terrain
{
// Layers baked from the tectonic plate model onto per-face cube rasters.
// Everything here is smooth by construction (TectonicField evaluates every
// near plate pair), so a fixed-resolution raster reproduces it faithfully; the
// terrain generator samples these instead of evaluating plates.
enum class BakedTectonicLayer : u8
{
    Convergence,
    Divergence,
    Transform,
    ConvergenceContinental,
    ConvergenceMixed,
    ConvergenceOceanic,
    PlateBiasMeters,
    CrustThicknessKm,
    CrustAge,
    GeologicalAge,
    // Tectonic uplift and subsidence only; hotspot chains are closed-form,
    // too small for the raster, and are added at sample time.
    UpliftMeters,
    SubsidenceMeters,
    Stress,
    // Subduction-arc and rift volcanism; hotspot volcanism is added at
    // sample time.
    VolcanismArc,
    PlateSpeedMeters,
    // 0 = oceanic, 1 = continental crust. Continuous and independent of plate
    // ids: one plate can carry both an ocean basin and a continent.
    ContinentalCrustFraction,
    // Subduction polarity: trench on the descending plate's side, arc inland
    // on the overriding plate's side (0..1).
    SubductionTrench,
    VolcanicArc,
    // Signed elevation (m) from tectonic structure alone: oceanic ridge swell
    // and age-depth subsidence, rift valley and shoulders, trench and arc.
    // The orogenic belt is not in it (convergence drives that separately).
    StructuralElevationMeters,
    // 0..1 fault/fracture intensity inside the deformation corridor.
    FractureDensity,
    // Wide convergence envelope terrain relief (mountain belts) is built from;
    // the Convergence layer itself is the narrow structure mask.
    OrogenEnvelope,
    Count
};

inline constexpr u32 kBakedTectonicLayerCount =
    static_cast<u32>(BakedTectonicLayer::Count);

inline constexpr u32 kBakedTectonicFaces = 6;
inline constexpr u32 kBakedTectonicMaxPlates = 24;

// One bilinearly interpolated sample of every layer (plate identity is the
// nearest texel's).
struct BakedTectonicTexel
{
    std::array<f32, kBakedTectonicLayerCount> values{};
    u8 plate{0};
    u8 neighbour{0};

    [[nodiscard]] f32 Get(BakedTectonicLayer layer) const noexcept
    {
        return values[static_cast<std::size_t>(layer)];
    }

    void Set(BakedTectonicLayer layer, f32 value) noexcept
    {
        values[static_cast<std::size_t>(layer)] = value;
    }
};

// Immutable baked tectonic rasters: six cube faces of `resolution` x
// `resolution` texels, each face carrying a one-texel gutter evaluated on the
// neighbouring faces' geometry so bilinear sampling is continuous across cube
// edges. Layers are quantized to 16 bits over a per-layer range.
class BakedTectonicRasters
{
public:
    static constexpr u32 kFormatVersion = 6;

    // Quantizes float layers. `layers[i]` holds 6 * (resolution + 2)^2 values
    // for layer i in face-major, row-major order with the gutter included;
    // `plate` and `neighbour` hold 6 * resolution^2 nearest-texel ids.
    [[nodiscard]] static BakedTectonicRasters FromFloats(
        u32 resolution,
        u64 recipeHash,
        const std::array<std::vector<f32>, kBakedTectonicLayerCount>& layers,
        std::vector<u8> plate,
        std::vector<u8> neighbour,
        const std::array<u8, kBakedTectonicMaxPlates>& plateContinental,
        u32 plateCount);

    // Rebuilds from already-quantized data (file load); validates sizes.
    [[nodiscard]] static BakedTectonicRasters FromQuantized(
        u32 resolution,
        u64 recipeHash,
        std::array<std::vector<u16>, kBakedTectonicLayerCount> layers,
        std::array<f32, kBakedTectonicLayerCount> rangeMinimum,
        std::array<f32, kBakedTectonicLayerCount> rangeMaximum,
        std::vector<u8> plate,
        std::vector<u8> neighbour,
        const std::array<u8, kBakedTectonicMaxPlates>& plateContinental,
        u32 plateCount);

    [[nodiscard]] u32 Resolution() const noexcept { return resolution_; }
    [[nodiscard]] u64 RecipeHash() const noexcept { return recipeHash_; }
    [[nodiscard]] u64 ContentHash() const noexcept { return contentHash_; }
    [[nodiscard]] u32 PlateCount() const noexcept { return plateCount_; }
    [[nodiscard]] bool PlateIsContinental(u32 plate) const noexcept
    {
        return plate < kBakedTectonicMaxPlates &&
               plateContinental_[plate] != 0U;
    }

    [[nodiscard]] BakedTectonicTexel Sample(
        const math::Double3& direction) const noexcept;

    // Hot path: only the two layers the elevation generator consumes.
    struct ConvergenceAndBias
    {
        // The orogen envelope (what terrain builds relief from).
        f32 convergence{0.0F};
        f32 plateBiasMeters{0.0F};
        f32 structuralElevationMeters{0.0F};
    };
    [[nodiscard]] ConvergenceAndBias SampleConvergenceAndBias(
        const math::Double3& direction) const noexcept;

    // Interleaved {convergence, bias, structural elevation} floats, 6 * (resolution + 2)^2 texels
    // with the gutter, for the GPU field generator.
    [[nodiscard]] std::vector<f32> BuildGpuConvergenceAndBias() const;

    // Raw access for serialization.
    [[nodiscard]] const std::vector<u16>& QuantizedLayer(
        BakedTectonicLayer layer) const noexcept
    {
        return layers_[static_cast<std::size_t>(layer)];
    }
    [[nodiscard]] f32 RangeMinimum(BakedTectonicLayer layer) const noexcept
    {
        return rangeMinimum_[static_cast<std::size_t>(layer)];
    }
    [[nodiscard]] f32 RangeMaximum(BakedTectonicLayer layer) const noexcept
    {
        return rangeMaximum_[static_cast<std::size_t>(layer)];
    }
    [[nodiscard]] const std::vector<u8>& PlateIds() const noexcept
    {
        return plate_;
    }
    [[nodiscard]] const std::vector<u8>& NeighbourIds() const noexcept
    {
        return neighbour_;
    }
    [[nodiscard]] const std::array<u8, kBakedTectonicMaxPlates>&
    PlateContinentalFlags() const noexcept
    {
        return plateContinental_;
    }

    [[nodiscard]] std::size_t ByteSize() const noexcept;

private:
    BakedTectonicRasters() = default;
    void Finalize();

    [[nodiscard]] f32 Dequantize(
        std::size_t layer, std::size_t index) const noexcept;

    u32 resolution_{0};
    u64 recipeHash_{0};
    u64 contentHash_{0};
    u32 plateCount_{0};
    std::array<std::vector<u16>, kBakedTectonicLayerCount> layers_{};
    std::array<f32, kBakedTectonicLayerCount> rangeMinimum_{};
    std::array<f32, kBakedTectonicLayerCount> rangeMaximum_{};
    std::vector<u8> plate_;
    std::vector<u8> neighbour_;
    std::array<u8, kBakedTectonicMaxPlates> plateContinental_{};
};

// Texel centre direction for a baked raster index, including the gutter
// (x and y in [-1, resolution]). Shared by the baker and the sampler so the
// two cannot drift apart.
[[nodiscard]] math::Double3 BakedTectonicTexelDirection(
    u32 face,
    i32 x,
    i32 y,
    u32 resolution) noexcept;
} // namespace orbit::terrain

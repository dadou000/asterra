#pragma once

#include <orbit/cloud_genera/CloudGenera.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace orbit::weather_lab
{
// The cloud types that fill the sky around a simulated storm are the WMO
// genera of the shared cloud_genera table. The storm itself (cumulonimbus) is
// simulated; every other genus is an authored procedural layer, one Volume
// object each, so each can be placed, hidden and regenerated on its own.
using CloudLayerKind = cloud_genera::CloudGenus;

// Number the Volume's Cloud Type property stores for each kind (0 is the
// storm: typed by height and density at draw time).
using cloud_genera::CloudTypeCode;

[[nodiscard]] std::string_view CloudLayerKindName(CloudLayerKind kind) noexcept;

// Parses "cirrus", "altocumulus", ... (case-insensitive); false if unknown.
[[nodiscard]] bool ParseCloudLayerKind(std::string_view name, CloudLayerKind& kind) noexcept;

// True for every genus the generator can draw (all but the simulated cumulonimbus).
[[nodiscard]] bool IsGeneratedCloudLayer(CloudLayerKind kind) noexcept;

// Vertical placement a kind should be given: centre height above the ground
// and half thickness, metres. The caller positions a Volume with these.
struct CloudLayerPlacement
{
    double centerAboveGroundMeters{0.0};
    double halfThicknessMeters{0.0};
    // Volume Extinction Scale (1/m per unit density) that gives the layer
    // its translucency: a storm core is ~0.8, a veil of ice a thousand times less.
    double extinctionScale{0.01};
    // Half width of a volume that resolves this genus' elements, metres.
    double halfExtentMeters{64000.0};
};

[[nodiscard]] CloudLayerPlacement DefaultCloudLayerPlacement(CloudLayerKind kind) noexcept;

struct CloudLayerSettings
{
    CloudLayerKind kind{CloudLayerKind::Cirrus};
    std::uint32_t seed{1U};
    // Grid of the density cache (x fastest, then y, then z up).
    std::uint32_t resolutionX{256U};
    std::uint32_t resolutionY{256U};
    std::uint32_t resolutionZ{256U};
    // Size of the Volume the grid is stretched over, half extents, metres.
    double halfExtentX{64000.0};
    double halfExtentY{64000.0};
    double halfExtentZ{1500.0};
    // 0 = clear sky, 1 = overcast layer.
    float coverage{0.55F};
    // Scales the baked density (the renderer reads 1 - exp(-optical depth)).
    float density{1.0F};
    // Direction the high wind draws ice into fibres along (x, y), any length.
    float windX{0.97F};
    float windY{0.23F};
    // Look style 0-7 for the genera that have variants (bit meanings per genus:
    // cumulus bit 0 more, smaller lobes, bit 1 towering, bit 2 sharp edges;
    // cirrus bit 0 hooked strands, bit 1 dense bundles, bit 2 finer strands).
    std::uint32_t style{0U};
};

// Baked density of one layer, in [0, 1], resolutionX * resolutionY *
// resolutionZ floats. Deterministic for a given settings value. A simulated
// genus (cumulonimbus) comes back empty.
[[nodiscard]] std::vector<float> GenerateCloudLayerDensity(const CloudLayerSettings& settings);
} // namespace orbit::weather_lab

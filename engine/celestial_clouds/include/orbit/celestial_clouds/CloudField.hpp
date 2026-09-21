#pragma once

#include <orbit/math/Vector.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/time/SimulationTime.hpp>

#include <memory>
#include <string>
#include <vector>

namespace orbit::celestial_clouds
{
enum class CloudSourceModel : u8
{
    ClimateProcedural = 0,
    Procedural = 1,
    Authored = 2,
    Imported = 3
};

struct CloudLayerParameters
{
    u64 semanticIdHigh{0};
    u64 semanticIdLow{0};
    CloudSourceModel sourceModel{
        CloudSourceModel::ClimateProcedural};
    f64 baseAltitudeMeters{1500.0};
    f64 topAltitudeMeters{6500.0};
    f64 coverageBias{0.0};
    f64 peakOpticalDepth{8.0};
    f64 singleScatteringAlbedo{0.999};
    f64 anisotropy{0.72};
    f64 densityExponent{1.35};
    f64 weatherScale{3.5};
    f64 detailScale{14.0};
    u64 seed{1};
    math::Double3 windAngularRadiansPerSecond{
        0.0, 0.0, 7.272205e-6};
    bool shadowParticipation{true};
    bool orbitalRepresentation{true};
};

struct CloudFieldConfig
{
    u32 faceResolution{65};
    f64 footprintScale{2.0};
    i64 timeQuantumMicroseconds{
        1'000'000};
};

struct CloudTexel
{
    f32 coverage{0.0F};
    f32 opticalDepth{0.0F};
    f32 singleScatteringAlbedo{0.999F};
    f32 anisotropy{0.72F};
};

struct CloudLayerField
{
    CloudLayerParameters parameters{};
    u64 fingerprint{0};
    std::vector<CloudTexel> texels;

    [[nodiscard]] const CloudTexel& At(
        u32 face,
        u32 x,
        u32 y,
        u32 faceResolution) const;
};

struct CloudFieldProduct
{
    u32 faceResolution{0};
    f64 sampleFootprintMeters{1.0};
    u64 climateRevision{0};
    i64 timeBucket{0};
    u64 fingerprint{0};
    std::vector<CloudLayerField> layers;

    [[nodiscard]] CloudTexel Sample(
        const math::Double3& unitDirection) const;
};

[[nodiscard]] u64 CloudFieldFingerprint(
    const terrain::TerrainSource* climateSource,
    f64 referenceRadiusMeters,
    const std::vector<CloudLayerParameters>& layers,
    time::SimulationTime atTime,
    const CloudFieldConfig& config = {});

[[nodiscard]] CloudFieldProduct BuildCloudField(
    const terrain::TerrainSource* climateSource,
    f64 referenceRadiusMeters,
    const std::vector<CloudLayerParameters>& layers,
    time::SimulationTime atTime,
    const CloudFieldConfig& config = {});

[[nodiscard]] f64 CloudShadowTransmittanceAtSurface(
    const CloudFieldProduct& field,
    f64 referenceRadiusMeters,
    const math::Double3& surfaceUnitDirection,
    const math::Double3& lightDirectionBody);

struct GpuCloudTexel
{
    f32 coverage{0.0F};
    f32 opticalDepth{0.0F};
    f32 singleScatteringAlbedo{0.999F};
    f32 anisotropy{0.72F};
};

class GpuCloudFieldProduct
{
public:
    GpuCloudFieldProduct(
        rhi::Device& device,
        const CloudFieldProduct& product);

    [[nodiscard]] rhi::Buffer& Buffer() noexcept;
    [[nodiscard]] u32 FaceResolution() const noexcept;
    [[nodiscard]] u32 LayerCount() const noexcept;
    [[nodiscard]] u64 Fingerprint() const noexcept;

private:
    std::unique_ptr<rhi::Buffer> buffer_;
    u32 faceResolution_{0};
    u32 layerCount_{0};
    u64 fingerprint_{0};
};
} // namespace orbit::celestial_clouds

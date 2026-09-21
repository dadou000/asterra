#pragma once

#include <orbit/math/Vector.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/universe/BodyRegistry.hpp>

#include <memory>
#include <vector>

namespace orbit::celestial_globe
{
struct MacroGlobeConfig
{
    u32 faceResolution{33};
    f64 footprintScale{1.5};
};

struct MacroGlobeVertex
{
    math::Double3 positionMeters{};
    math::Double3 normal{};
    f64 elevationMeters{0.0};
};

struct MacroGlobeMesh
{
    std::vector<MacroGlobeVertex> vertices;
    std::vector<u32> indices;
    f64 referenceRadiusMeters{1.0};
    f64 minimumRadiusMeters{1.0};
    f64 maximumRadiusMeters{1.0};
    f64 sampleFootprintMeters{1.0};
    u64 sourceRevision{0};
    u64 fingerprint{0};
};

[[nodiscard]] MacroGlobeMesh BuildMacroGlobe(
    const terrain::TerrainSource& source,
    const universe::BodyShape& shape,
    const MacroGlobeConfig& config = {});

struct GpuMacroGlobeVertex
{
    math::Float3 positionNormalized{};
    math::Float3 normal{};
};

class GpuMacroGlobeProduct
{
public:
    GpuMacroGlobeProduct(
        rhi::Device& device,
        const MacroGlobeMesh& mesh);

    [[nodiscard]] rhi::Buffer& VertexBuffer() noexcept;
    [[nodiscard]] rhi::Buffer& IndexBuffer() noexcept;
    [[nodiscard]] u32 IndexCount() const noexcept;
    [[nodiscard]] f64 ReferenceRadiusMeters() const noexcept;
    [[nodiscard]] u64 Fingerprint() const noexcept;

private:
    std::unique_ptr<rhi::Buffer> vertices_;
    std::unique_ptr<rhi::Buffer> indices_;
    u32 indexCount_{0};
    f64 referenceRadiusMeters_{1.0};
    u64 fingerprint_{0};
};
} // namespace orbit::celestial_globe

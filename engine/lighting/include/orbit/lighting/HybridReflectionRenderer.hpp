#pragma once

#include <orbit/lighting/LightingView.hpp>
#include <orbit/lighting/ReflectionScene.hpp>
#include <orbit/lighting/ScreenSpaceFinalGather.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <memory>
#include <string>
#include <map>
#include <vector>

namespace orbit::lighting
{
// The actual runtime sources, also compiled by the headless shader gate.
[[nodiscard]] std::string BuildHybridReflectionShaderSource(bool hardware = false);
[[nodiscard]] std::string BuildReflectionCompositeShaderSource();

struct HybridReflectionSettings
{
    f32 maximumScreenTraceRoughness{0.45F};
    f32 mirrorRoughness{0.08F};
    f32 traceRadiusMeters{40.0F};
    f32 thicknessMeters{0.12F};
    f32 cacheStrength{1.0F};
    u32 maximumSteps{24U};
    f32 maximumSmoothRoughness{0.25F};
    bool exactTriangles{true};
    bool hardwareRayQueries{true};
    bool temporal{true};
    u32 debugView{0U}; // 0 scene, 1 reflected radiance, 2 hit distance

};

class HybridReflectionRenderer
{
public:
    HybridReflectionRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);

    // Once per frame, AFTER this slot's submission fence completed. A retired
    // generation survives until all potentially referring slots completed.
    void BeginFrame(u32 completedSlot, u32 framesInFlight);

    void Resolve(
        rhi::CommandList& commands,
        rhi::Texture& sceneColor,
        rhi::Texture& surfaceBaseRoughness,
        rhi::Texture& surfaceNormalMetallic,
        // Class 3 (authored proxy) pixels skip the cache fallback.
        rhi::Texture& surfaceEmissionClass,
        rhi::Texture& depth,
        rhi::Buffer& radianceCells,
        rhi::Buffer& radianceLevels,
        u32 radianceLevelCount,
        rhi::Texture& targetSceneColor,
        u32 width,
        u32 height,
        const LightingView& view,
        f32 qualityScale,
        const HybridReflectionSettings& settings = {},
        // The merged mesh distance field: where the screen cannot answer, a
        // smooth surface reflects the room it finds there instead of the
        // radiance cache's low-frequency sky. Null: cache only.
        const SdfGatherInput* sdf = nullptr,
        const ReflectionSceneInput* exact = nullptr);

private:
    struct History
    {
        std::unique_ptr<rhi::Texture> raw;
        std::array<std::unique_ptr<rhi::Texture>, 2> radiance;
        std::array<std::unique_ptr<rhi::Texture>, 2> metadata;
        LightingView view;
        u32 index{0U};
        bool valid{false};
        u64 geometryRevision{0U};
        u64 lastTick{0U};
        std::array<math::Float4, 4> lighting{};
    };
    struct RetiredParameters
    {
        std::unique_ptr<rhi::Buffer> buffer;
        u64 pendingFrames{0U};
    };
    rhi::Device& device_;
    std::map<const rhi::Texture*, History> histories_;
    std::vector<RetiredParameters> parameters_;
    std::vector<std::pair<History, u64>> retiredHistories_;
    u64 tick_{0U};
    u64 pendingFramesMask_{~0ULL};
    std::unique_ptr<rhi::ComputePipeline> compositePipeline_;
    std::unique_ptr<rhi::ComputePipeline> hardwarePipeline_;
    std::unique_ptr<rhi::ComputePipeline> pipeline_;
    std::unique_ptr<rhi::Buffer> dummySdf_;
};
} // namespace orbit::lighting

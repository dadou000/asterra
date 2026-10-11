#pragma once

#include <orbit/celestial_atmosphere/Atmosphere.hpp>
#include <orbit/celestial_atmosphere/AtmosphereRenderer.hpp>
#include <orbit/celestial_clouds/CloudField.hpp>
#include <orbit/core/Types.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <memory>
#include <string>
#include <span>
#include <vector>

namespace orbit::celestial_clouds
{
// Cloud lab: when enabled the weather is replaced by one isolated cloud of the
// given type at a point on the body, with a taller shell, so its vertical
// development, shape and self-shadowing can be inspected in isolation.
struct CloudLab
{
    bool enabled{false};
    // Unit direction (body-fixed frame) of the cloud's centre.
    math::Double3 centerDirection{1.0, 0.0, 0.0};
    f32 radiusMeters{6000.0F};
    f32 type{1.0F};
    f32 coverage{0.85F};
    f32 cirrus{0.0F};
    f32 precipitation{0.3F};
    // Multiplies the vertical extent of the cloud shell.
    f32 heightScale{1.0F};
    // Life cycle: 0 towering cumulus, 0.3 growing cumulonimbus (cauliflower, no
    // anvil), 0.6 mature (glaciated top, anvil spreading), 0.9 dissipating (tower
    // collapsing, broad ragged anvil with mammatus).
    f32 maturity{0.6F};
    // 0 isolated single cell, 0.5 multicell cluster of cells of different ages,
    // 1 organised (large coherent updraft, wide anvil).
    f32 organisation{0.2F};
    // Multiplies the extinction (denser clouds shade themselves harder).
    f32 density{1.0F};
    // Coverage of a patchy thin-cirrus sheet on the anti-sun side of the cell (0 = none): it
    // sits where the storm's shadow falls, to see clouds shadowing cirrus.
    f32 cirrusSheet{0.0F};
    u32 seed{1U};
};

// Per-pixel ray-marched cloud shell for near-field and surface views, where
// the macro-globe appearance composite is not drawn. Reads the same GPU cloud
// field the orbital composite is built from, so both views agree on where
// the weather is. Composites sceneColor * T + L into target; run it before
// the atmosphere pass so aerial perspective is applied over the clouds.
class CloudRenderer
{
public:
    CloudRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);
    ~CloudRenderer();

    // One view's sun light volume: a cache of the optical depth towards the sun around that view's
    // camera (see kVolumeMath). It is tied to one camera (its anchor, toroidal window and voxel
    // generation), so every view that draws clouds needs its own: sharing one made two cameras
    // re-anchor it back and forth every frame, and no voxel ever stayed valid.
    struct LightVolume
    {
        std::unique_ptr<rhi::Buffer> buffer;
        math::Double3 anchor{};
        bool anchorSet{false};
        bool active{false};
        u32 generation{0U};

        // Exact skip of redundant refreshes (see UpdateLightVolume): a hash of every input the
        // update reads, and how many consecutive frames it has been unchanged.
        u64 inputFingerprint{0U};
        u32 settledFrames{0U};
        // True when the last UpdateLightVolume skipped its dispatch because every voxel already
        // holds the result for the current inputs.
        bool refreshSkipped{false};
        // Tests set this to false to get the original refresh-forever behaviour as a reference.
        bool allowRefreshSkip{true};
    };

    // A point light that scatters into the clouds (ORBIT_CLOUD_POINT_LIGHTS=1): a lightning flash, street lamps, a lamp
    // inside a cloud. positionMeters is in the same body-centric frame as the view's camera position. `intensity` is the
    // radiant intensity per channel in scene-linear units, W/sr as a fraction of the 1361 W/m2 reference (so irradiance
    // at distance d is intensity / d^2 and equals the sun's when they match). At most 16 are used.
    struct PointLight
    {
        math::Double3 positionMeters{};
        math::Float3 intensity{};
    };
    static constexpr u32 kMaxPointLights = 16U;

    // Call once per frame before recording, with the frame slot that has completed and the number of frames in flight,
    // so per-frame light buffers are retired only after the GPU is done with them (same contract as the glass renderer).
    void BeginFrame(u32 completedSlot, u32 framesInFlight);
    [[nodiscard]] bool PointLightsEnabled() const noexcept { return pointLights_; }

    // A simulated storm domain (engine/weather_gpu): 32 placement floats then extinction per cell (see StormSigma in the shader).
    // The buffer must stay alive and in ResourceState::ShaderResource while the clouds draw; nullptr restores the procedural clouds.
    // Inside the domain the simulated extinction replaces the procedural density in the march, the cloud shadow and the sun light
    // volume alike, so the storm shades itself and the ground. CLOUD_STORM=0 (shader defines) turns the lookup off for A/B work.
    void SetStormBuffer(rhi::Buffer* buffer) noexcept { storm_ = buffer; }

    // Developer hook (see tools/orbit_ab.py): the march, shadow and volume shaders are recompiled with
    // "NAME=VALUE;NAME=VALUE" as #define lines (tunables such as CLOUD_MAX_STEPS). Returns the compile error, or an empty
    // string; on error the previous shaders stay in use. Safe between frames: the replaced pipelines are retired by the RHI.
    [[nodiscard]] std::string SetShaderDefines(const std::string& defines);
    [[nodiscard]] const std::string& ShaderDefines() const noexcept { return defines_; }

    // Split view (clouds.split_set): the left of the viewport shows the march with ShaderDefines(), the right a march and resolve
    // compiled with these defines (the shadow and light-volume shaders stay as they are). `position` is the divider, 0..1 of the
    // viewport width. Returns the compile error or an empty string; on error the split stays as it was.
    [[nodiscard]] std::string SetCompareDefines(const std::string& defines, f32 position);
    void ClearCompare();
    [[nodiscard]] bool CompareActive() const noexcept { return compareActive_; }
    [[nodiscard]] const std::string& CompareDefines() const noexcept { return definesB_; }
    [[nodiscard]] f32 ComparePosition() const noexcept { return comparePosition_; }
    [[nodiscard]] u32 CompareCheckerboardMode() const noexcept { return checkerboardB_; }
    // True while CLOUD_CHECKERBOARD=1: the march target is half the cloud size in each dimension (one pixel of every 2x2 block per frame).
    [[nodiscard]] bool CheckerboardEnabled() const noexcept { return checkerboard_ != 0U; }
    // 2 while CLOUD_CHECKERBOARD=2: half the pixels per frame, the march target is half the width and the full height.
    [[nodiscard]] u32 CheckerboardMode() const noexcept { return checkerboard_; }

    CloudRenderer(const CloudRenderer&) = delete;
    CloudRenderer& operator=(const CloudRenderer&) = delete;

    // Marches the cloud shell into 'target' (RGBA16F, usually half the scene
    // resolution) as premultiplied radiance in rgb and transmittance in alpha.
    // 'depth' is the full-resolution scene depth that bounds each ray. The
    // camera, sun and irradiance reuse the atmosphere view (body-fixed frame).
    // Only the first cloud layer is drawn.
    void Draw(
        rhi::CommandList& commands,
        const LightVolume& volume,
        rhi::Texture& depth,
        celestial_atmosphere::GpuAtmosphereLuts& luts,
        GpuCloudFieldProduct& field,
        rhi::Texture& target,
        u32 width,
        u32 height,
        f64 referenceRadiusMeters,
        const CloudLayerParameters& layer,
        const celestial_atmosphere::AtmosphereParameters& atmosphere,
        const celestial_atmosphere::AtmosphereRenderView& view,
        const CloudLab& lab = {},
        // Varies the march jitter frame to frame; the temporal Resolve averages the frames.
        u32 frameIndex = 0U,
        // Scales the crepuscular rays (sunlight removed from cloud-shadowed air); 0 = off.
        f32 godrayStrength = 1.0F,
        // > 0 draws a horizontal slice of the sun light volume at this altitude (m) as a heatmap.
        f32 volumeDebugAltitude = 0.0F,
        // Ignored unless PointLightsEnabled().
        std::span<const PointLight> pointLights = {},
        u32 variant = 0U,        // 1: the split view's second march (SetCompareDefines)
        f32 regionLeft = 0.0F,   // the part of the target drawn, as fractions of its width
        f32 regionRight = 1.0F);

    [[nodiscard]] std::unique_ptr<LightVolume> CreateLightVolume() const;

    // Refreshes the sun light volume: a compute pass that updates a few voxels per frame. Run it
    // before Draw in the same command list; Draw reads the volume when it is enabled.
    void UpdateLightVolume(
        rhi::CommandList& commands,
        LightVolume& volume,
        GpuCloudFieldProduct& field,
        f64 referenceRadiusMeters,
        const CloudLayerParameters& layer,
        const celestial_atmosphere::AtmosphereParameters& atmosphere,
        const celestial_atmosphere::AtmosphereRenderView& view,
        const CloudLab& lab,
        u32 frameIndex,
        bool enabled);

    // Sun transmittance (red channel, 1 = fully lit) through the cloud shell at
    // the surface each pixel sees, from the same density the march draws. The
    // target is a reduced-resolution RGBA16F texture sampled by the direct
    // lighting pass so terrain receives cloud shadows.
    void DrawShadow(
        rhi::CommandList& commands,
        rhi::Texture& depth,
        GpuCloudFieldProduct& field,
        rhi::Texture& target,
        u32 width,
        u32 height,
        f64 referenceRadiusMeters,
        const CloudLayerParameters& layer,
        const celestial_atmosphere::AtmosphereRenderView& view,
        const CloudLab& lab = {});

    // A camera as the temporal resolve needs it (body-fixed frame).
    struct CloudResolveView
    {
        math::Double3 cameraPositionMeters{};
        math::Float3 forward{0.0F, 0.0F, 1.0F};
        math::Float3 up{0.0F, 1.0F, 0.0F};
        f32 verticalFovRadians{1.0F};
        f32 aspect{1.0F};
    };

    // Temporal accumulation of the march: writes 'target' = history (reprojected from
    // 'previous' to 'now' through the cloud shell, clamped to the neighbourhood of
    // 'current') blended with 'current' by currentWeight. With no valid history the
    // current frame is copied. 'current', 'history' and 'target' share one size.
    void Resolve(
        rhi::CommandList& commands,
        rhi::Texture& current,
        rhi::Texture& history,
        rhi::Texture& target,
        u32 width,
        u32 height,
        f64 shellRadiusMeters,
        const CloudResolveView& now,
        const CloudResolveView& previous,
        bool historyValid,
        f32 currentWeight,
        u32 frameIndex = 0U,
        u32 variant = 0U,
        f32 regionLeft = 0.0F,
        f32 regionRight = 1.0F);

    // Blends the reduced-resolution result over 'target' (scene colour):
    // scene * T + L, bilinearly upsampled.
    void Composite(
        rhi::CommandList& commands,
        rhi::Texture& clouds,
        rhi::Texture& target,
        u32 width,
        u32 height);

    // The split view's composite: cloud target `cloudsLeft` left of `position` (0..1 of the width), `cloudsRight` right of it, and a
    // white divider line.
    void CompositeSplit(
        rhi::CommandList& commands,
        rhi::Texture& cloudsLeft,
        rhi::Texture& cloudsRight,
        rhi::Texture& target,
        u32 width,
        u32 height,
        f32 position);

private:
    [[nodiscard]] static u32 CheckerboardModeOf(const std::string& defineText);
    // Compiles the march, shadow and volume shaders and swaps them in; returns an error text or an empty string.
    [[nodiscard]] std::string RebuildShaderPipelines(const std::string& defineText);
    const shader::Compiler* compiler_{nullptr};
    std::string farField_;
    std::string defines_;
    u32 checkerboard_{0U};
    // Split view: the second march/resolve pair, and the composite that shows both.
    bool compareActive_{false};
    f32 comparePosition_{0.5F};
    std::string definesB_;
    u32 checkerboardB_{0U};
    std::unique_ptr<rhi::GraphicsPipeline> pipelineB_;
    std::unique_ptr<rhi::GraphicsPipeline> resolvePipelineB_;
    std::unique_ptr<rhi::GraphicsPipeline> compositeSplitPipeline_;
    std::unique_ptr<rhi::GraphicsPipeline> pipeline_;
    std::unique_ptr<rhi::GraphicsPipeline> compositePipeline_;
    std::unique_ptr<rhi::GraphicsPipeline> resolvePipeline_;
    std::unique_ptr<rhi::GraphicsPipeline> shadowPipeline_;
    // Baked 32^3 tileable cellular noise (base shape + erosion octaves).
    std::unique_ptr<rhi::Buffer> noise_;
    std::unique_ptr<rhi::Buffer> stormDummy_;
    rhi::Buffer* storm_{nullptr};
    [[nodiscard]] rhi::Buffer& StormBuffer() const noexcept { return storm_ != nullptr ? *storm_ : *stormDummy_; }
    // Sun light volume (see UpdateLightVolume): voxels, the pipeline that fills them and the
    // tangent-frame anchor that makes toroidal addressing valid.
    std::unique_ptr<rhi::ComputePipeline> volumePipeline_;
    rhi::Device* device_{nullptr};

    // Point lights (ORBIT_CLOUD_POINT_LIGHTS): a fourth shader buffer, rewritten every Draw. Buffers are kept alive until
    // every frame in flight that could read them has completed (see BeginFrame).
    struct RetiredBuffer
    {
        std::unique_ptr<rhi::Buffer> buffer;
        u64 pendingFrames{0U};
    };
    [[nodiscard]] rhi::Buffer& UploadPointLights(std::span<const PointLight> lights, const math::Double3& cameraPositionMeters);
    bool pointLights_{false};
    std::vector<RetiredBuffer> lightBuffers_;
    u64 pendingFramesMask_{~0ULL};
};
} // namespace orbit::celestial_clouds

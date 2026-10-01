#include <orbit/profiler/Profiler.hpp>
#include <orbit/terrain_render/TerrainPreviewRenderer.hpp>
#include <orbit/terrain_render/SurfaceEffectGpuBinding.hpp>
#include <orbit/terrain_render/SurfaceEffectShader.hpp>
#include <orbit/terrain_render/WaterVolumeShader.hpp>
#include "ClipmapVertexShader.hpp"
#include "TerrainSurfaceShader.hpp"

#include <orbit/math/Matrix.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain_stream/TerrainMorphRefresh.hpp>
#include <orbit/terrain_stream/TerrainSampleStreamer.hpp>
#include <orbit/terrain_stream/ToroidalResidency.hpp>
#include <orbit/terrain_view/ClipmapTracker.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace orbit::terrain_render
{
namespace
{
[[nodiscard]] math::Float3 ToObserverLocal(
    const math::Double3& vector,
    const world::SurfaceFrame& observerFrame) noexcept
{
    return {
        static_cast<f32>(math::Dot(vector, observerFrame.east)),
        static_cast<f32>(math::Dot(vector, observerFrame.up)),
        static_cast<f32>(math::Dot(vector, observerFrame.north))
    };
}

[[nodiscard]] std::array<u32, 56> BuildDrawConstants(
    const math::Mat4& matrix,
    const f32 planetRadiusMeters,
    const f32 observerRadiusMeters,
    const terrain_view::ClipmapLevel& level,
    const terrain_view::ClipmapLevel* coarserLevel,
    const terrain_view::ClipmapLevelMotion& motion,
    const terrain_view::ClipmapLevelMotion* finerMotion,
    const terrain_stream::LevelResidencyUpdate& residency,
    const world::SurfaceFrame& observerFrame,
    const u32 levelIndex,
    const bool debugLodColorEnabled,
    const bool debugSideCutEnabled,
    const bool drySurface,
    const f32 seaLevelMeters) noexcept
{
    std::array<u32, 56> result{};

    static_assert(sizeof(matrix.values) == 16U * sizeof(u32));
    std::memcpy(result.data(), matrix.values.data(), sizeof(matrix.values));

    const math::Float3 centerUp = ToObserverLocal(motion.surfaceFrame.up, observerFrame);
    const math::Float3 centerEast = ToObserverLocal(motion.surfaceFrame.east, observerFrame);
    const math::Float3 centerNorth = ToObserverLocal(motion.surfaceFrame.north, observerFrame);

    math::Double2 innerHoleCenterOffset{};
    if (finerMotion != nullptr && level.innerHoleHalfExtentMeters > 0.0)
    {
        innerHoleCenterOffset = {
            finerMotion->centerOffsetMeters.x - motion.centerOffsetMeters.x,
            finerMotion->centerOffsetMeters.y - motion.centerOffsetMeters.y
        };
    }

    const auto store = [&result](const u32 index, const f32 value)
    {
        result[index] = std::bit_cast<u32>(value);
    };

    store(16, planetRadiusMeters);
    store(17, observerRadiusMeters);
    store(18, static_cast<f32>(level.sampleSpacingMeters));
    store(19, static_cast<f32>(level.gridResolution));

    store(20, centerUp.x);
    store(21, centerUp.y);
    store(22, centerUp.z);
    store(23, static_cast<f32>(residency.originX));

    store(24, centerEast.x);
    store(25, centerEast.y);
    store(26, centerEast.z);
    store(27, static_cast<f32>(residency.originY));

    store(28, centerNorth.x);
    store(29, centerNorth.y);
    store(30, centerNorth.z);
    store(31, static_cast<f32>(level.morphStartHalfExtentMeters));

    store(32, static_cast<f32>(level.morphEndHalfExtentMeters));
    store(33, static_cast<f32>(innerHoleCenterOffset.x));
    store(34, coarserLevel != nullptr ? 1.0F : 0.0F);
    store(35, static_cast<f32>(level.innerHoleHalfExtentMeters));

    store(36, static_cast<f32>(levelIndex));
    store(37, debugLodColorEnabled ? 1.0F : 0.0F);
    store(38, debugSideCutEnabled ? 1.0F : 0.0F);
    store(39, static_cast<f32>(innerHoleCenterOffset.y));

    store(40, static_cast<f32>(motion.centerOffsetMeters.x));
    store(41, static_cast<f32>(motion.centerOffsetMeters.y));
    store(42, drySurface ? 1.0F : 0.0F);
    store(43, seaLevelMeters);

    store(44, static_cast<f32>(observerFrame.east.x));
    store(45, static_cast<f32>(observerFrame.east.y));
    store(46, static_cast<f32>(observerFrame.east.z));

    store(48, static_cast<f32>(observerFrame.up.x));
    store(49, static_cast<f32>(observerFrame.up.y));
    store(50, static_cast<f32>(observerFrame.up.z));

    store(52, static_cast<f32>(observerFrame.north.x));
    store(53, static_cast<f32>(observerFrame.north.y));
    store(54, static_cast<f32>(observerFrame.north.z));

    return result;
}

constexpr const char* kVertexShader = detail::kClipmapVertexShader;


constexpr auto kPixelShader = detail::kTerrainSurfacePixelShader;
} // namespace

class TerrainPreviewRenderer::Impl
{
public:
    Impl(
        rhi::Device& device,
        const shader::Compiler& shaderCompiler,
        const world::PlanetDefinition& planet,
        terrain_gpu::GpuFieldGenerator& gpuFieldGenerator,
        const world::WorldPosition& observer,
        TerrainPreviewConfig config,
        terrain_gpu::GpuRegionDelta* regionDeltaComposite,
        terrain_region::DerivedTerrainRegionCache* hydrologyRegionCache)
        : device_(device),
          planet_(planet),
          gpuFieldGenerator_(gpuFieldGenerator),
          physicalPageComposite_(device, shaderCompiler),
          regionDeltaComposite_(regionDeltaComposite),
          hydrologyRegionCache_(hydrologyRegionCache),
          config_(std::move(config)),
          surfaceEffects_(device, config_.framesInFlight),
          baseClipmapConfig_(config_.clipmap),
          layout_(terrain_view::BuildClipmapLayout(config_.clipmap, observer)),
          tracker_(planet_, config_.clipmap),
          residency_(config_.clipmap),
          levels_(config_.clipmap.levelCount)
    {
        if (math::Length(observer.meters) <= planet_.radiusMeters)
            throw std::invalid_argument(
                "Orbit terrain preview observer must be above the planet surface.");
        if (config_.framesInFlight == 0)
            throw std::invalid_argument(
                "Orbit terrain preview requires at least one frame in flight.");

        CreateSharedTopology();
        CreateLevelBuffers();
        CreateWaterOpticsBuffers();
        CreatePipeline(shaderCompiler);
        CreateWaterPipeline(shaderCompiler);
        InitializeBlocking(observer);
    }

    ~Impl() = default;

    void SetDrySurface(const bool dry) noexcept
    {
        config_.drySurface = dry;
    }

    void SetWaterOptics(const TerrainWaterOptics& optics) noexcept
    {
        waterOptics_ = optics;
    }

    void UpdateObserver(const world::WorldPosition& observer)
    {
        SetObserverView(observer);
        desiredObserver_ = observer;
        desiredCoverageTier_ = SelectCoverageTier(observer, desiredCoverageTier_);
        ++desiredGeneration_;
        ServiceStreaming();
    }

    void SetDebugVisuals(
        const bool lodColorEnabled,
        const bool sideCutEnabled) noexcept
    {
        debugLodColorEnabled_ = lodColorEnabled;
        debugSideCutEnabled_ = sideCutEnabled;
    }

    void SetGenerationFrozen(const bool frozen) noexcept
    {
        generationFrozen_ = frozen;
    }

    void SetPhysicalPages(
        const std::span<const terrain_gpu::GpuPhysicalSurfacePage> pages,
        const u64 generation)
    {
        if (physicalPageGeneration_ == generation &&
            physicalPages_.size() == pages.size())
        {
            bool same = true;
            for (std::size_t index = 0U; index < pages.size(); ++index)
            {
                const auto& left = physicalPages_[index];
                const auto& right = pages[index];
                if (!(left.address == right.address) ||
                    left.resolution != right.resolution ||
                    left.samples != right.samples)
                {
                    same = false;
                    break;
                }
            }
            if (same)
                return;
        }

        physicalPages_.assign(pages.begin(), pages.end());
        physicalPageGeneration_ = generation;
        RecordPhysicalPageRefresh();
    }

    void SetSurfaceEffects(const std::span<const SurfaceEffectGpuStamp> effects)
    {
        surfaceEffects_.Set(effects);
    }

    [[nodiscard]] math::Mat4 ViewProjection(
        const TerrainPreviewCamera& camera,
        const u32 targetWidth,
        const u32 targetHeight) const
    {
        const f32 aspect = static_cast<f32>(targetWidth) /
            static_cast<f32>(targetHeight);
        math::Float3 cameraForward = math::Normalize(camera.forward);
        if (math::LengthSquared(cameraForward) <= 1.0e-8F)
            cameraForward = math::Normalize({0.0F, -0.28F, 1.0F});
        math::Float3 cameraUp = math::Normalize(camera.up);
        if (math::LengthSquared(cameraUp) <= 1.0e-8F ||
            math::LengthSquared(math::Cross(cameraUp, cameraForward)) <= 1.0e-8F)
            cameraUp = {0.0F, 1.0F, 0.0F};

        const math::Mat4 view = math::LookAtLH(
            {0.0F, 0.0F, 0.0F}, cameraForward, cameraUp);
        const math::Mat4 projection = math::PerspectiveReverseZLH(
            camera.verticalFovRadians > 0.0F
                ? camera.verticalFovRadians : config_.verticalFovRadians,
            aspect,
            camera.nearPlaneMeters > 0.0F
                ? camera.nearPlaneMeters : config_.nearPlaneMeters,
            camera.farPlaneMeters > 0.0F
                ? camera.farPlaneMeters : config_.farPlaneMeters);
        return math::Multiply(view, projection);
    }

    void DrawWater(
        rhi::CommandList& commandList,
        const u32 frameIndex,
        const u32 targetWidth,
        const u32 targetHeight,
        const TerrainPreviewCamera& camera,
        rhi::Texture& terrainDepth)
    {
        if (frameIndex >= config_.framesInFlight)
            throw std::out_of_range(
                "Orbit terrain frame index exceeds configured frames in flight.");
        if (targetWidth == 0 || targetHeight == 0 || waterOptics_.opacity <= 0.0F)
            return;

        const math::Mat4 mvp =
            ViewProjection(camera, targetWidth, targetHeight);

        commandList.SetViewport({
            .x = 0.0F,
            .y = 0.0F,
            .width = static_cast<f32>(targetWidth),
            .height = static_cast<f32>(targetHeight),
            .minDepth = 0.0F,
            .maxDepth = 1.0F
        });
        commandList.SetScissor({
            .left = 0,
            .top = 0,
            .right = static_cast<i32>(targetWidth),
            .bottom = static_cast<i32>(targetHeight)
        });
        commandList.SetGraphicsPipeline(*waterPipeline_);
        BindWaterOptics(
            commandList,
            frameIndex,
            camera.nearPlaneMeters > 0.0F
                ? camera.nearPlaneMeters : config_.nearPlaneMeters,
            camera.farPlaneMeters > 0.0F
                ? camera.farPlaneMeters : config_.farPlaneMeters);
        commandList.SetGraphicsTexture(0, terrainDepth);

        for (u32 levelIndex = 0;
             levelIndex < static_cast<u32>(levels_.size());
             ++levelIndex)
        {
            const terrain_view::ClipmapLevel& level =
                layout_.levels[levelIndex];
            const terrain_view::ClipmapLevel* coarserLevel =
                levelIndex + 1U < static_cast<u32>(levels_.size())
                    ? &layout_.levels[levelIndex + 1U]
                    : nullptr;
            const terrain_view::ClipmapLevelMotion* finerMotion =
                levelIndex > 0U ? &motion_.levels[levelIndex - 1U] : nullptr;

            const auto constants = BuildDrawConstants(
                mvp,
                static_cast<f32>(planet_.radiusMeters),
                observerRadiusMeters_,
                level,
                coarserLevel,
                motion_.levels[levelIndex],
                finerMotion,
                residencyUpdate_.levels[levelIndex],
                observerFrame_,
                levelIndex,
                false,
                false,
                config_.drySurface,
                waterOptics_.seaLevelMeters);

            commandList.SetGraphicsConstants(constants);
            commandList.SetGraphicsBuffer(
                0, *levels_[levelIndex].gpuSampleBuffer);
            commandList.Draw(patchVertexCount_);
        }
    }

    void Draw(
        rhi::CommandList& commandList,
        const u32 frameIndex,
        const u32 targetWidth,
        const u32 targetHeight,
        const TerrainPreviewCamera& camera)
    {
        ServiceStreaming();
        stats_.uploadedBytesLastFrame = 0;
        if (stats_.rebaseCount > 0)
        {
            stats_.secondsSinceLastRebase = std::chrono::duration<f64>(
                std::chrono::steady_clock::now() - lastRebaseTime_).count();
        }
        stats_.drawCallsLastFrame = 0;

        if (frameIndex >= config_.framesInFlight)
            throw std::out_of_range(
                "Orbit terrain frame index exceeds configured frames in flight.");
        if (targetWidth == 0 || targetHeight == 0)
            return;

        const math::Mat4 mvp =
            ViewProjection(camera, targetWidth, targetHeight);

        commandList.SetViewport({
            .x = 0.0F,
            .y = 0.0F,
            .width = static_cast<f32>(targetWidth),
            .height = static_cast<f32>(targetHeight),
            .minDepth = 0.0F,
            .maxDepth = 1.0F
        });
        commandList.SetScissor({
            .left = 0,
            .top = 0,
            .right = static_cast<i32>(targetWidth),
            .bottom = static_cast<i32>(targetHeight)
        });
        commandList.SetGraphicsPipeline(*pipeline_);
        surfaceEffects_.Bind(commandList, frameIndex);

        for (u32 levelIndex = 0;
             levelIndex < static_cast<u32>(levels_.size());
             ++levelIndex)
        {
            stats_.uploadedBytesLastFrame +=
                PrepareLevelFrame(commandList, levelIndex);

            const terrain_view::ClipmapLevel& level =
                layout_.levels[levelIndex];
            const terrain_view::ClipmapLevel* coarserLevel =
                levelIndex + 1U < static_cast<u32>(levels_.size())
                    ? &layout_.levels[levelIndex + 1U]
                    : nullptr;
            const terrain_view::ClipmapLevelMotion* finerMotion =
                levelIndex > 0U ? &motion_.levels[levelIndex - 1U] : nullptr;

            const auto constants = BuildDrawConstants(
                mvp,
                static_cast<f32>(planet_.radiusMeters),
                observerRadiusMeters_,
                level,
                coarserLevel,
                motion_.levels[levelIndex],
                finerMotion,
                residencyUpdate_.levels[levelIndex],
                observerFrame_,
                levelIndex,
                debugLodColorEnabled_,
                debugSideCutEnabled_,
                config_.drySurface,
                waterOptics_.seaLevelMeters);

            commandList.SetGraphicsConstants(constants);
            commandList.SetGraphicsBuffer(
                0, *levels_[levelIndex].gpuSampleBuffer);
            commandList.Draw(patchVertexCount_);
            ++stats_.drawCallsLastFrame;
        }

        stats_.cumulativeUploadedBytes += stats_.uploadedBytesLastFrame;
    }

    [[nodiscard]] u32 VertexCount() const noexcept
    {
        const u64 total = static_cast<u64>(patchVertexCount_) *
            static_cast<u64>(config_.clipmap.levelCount);
        return static_cast<u32>(std::min<u64>(
            total, std::numeric_limits<u32>::max()));
    }

    [[nodiscard]] u32 IndexCount() const noexcept { return 0U; }

    [[nodiscard]] const TerrainStreamingStats& StreamingStats() const noexcept
    {
        return stats_;
    }

private:
    struct CandidateState
    {
        u32 coverageTier{0};
        terrain_view::ClipmapConfig clipmapConfig{};
        terrain_view::ClipmapLayout layout;
        terrain_view::ClipmapTracker tracker;
        terrain_stream::ToroidalResidency residency;
        terrain_view::ClipmapMotionUpdate motion;
        terrain_stream::ResidencyUpdate residencyUpdate;
        std::vector<terrain_stream::TerrainSampleRequest> requests;
    };

    struct DirtyUpdate
    {
        u64 serial{0};
        terrain_stream::TerrainSampleRequest request;
    };

    // Terrain sample residency is persistent per LOD, not per CPU frame.
    // All current RHI frame submissions for this renderer use the same ordered
    // graphics queue. A later frame's UAV update therefore executes after every
    // earlier frame's SRV read of this buffer, while avoiding N-way VRAM and
    // compute duplication for N frames in flight. Truly frame-local host writes
    // (surface effects, constants, etc.) remain ring-buffered separately.
    struct LevelGpuState
    {
        std::unique_ptr<rhi::Buffer> gpuSampleBuffer;
        std::deque<DirtyUpdate> dirtyUpdates;
        u64 appliedSerial{0};
        u64 currentSerial{0};
    };

    void CreateSharedTopology()
    {
        const u64 cellsPerAxis =
            static_cast<u64>(config_.clipmap.gridResolution) - 1ULL;
        const u64 vertexCount = cellsPerAxis * cellsPerAxis * 6ULL;
        patchVertexCount_ = static_cast<u32>(std::min<u64>(
            vertexCount, std::numeric_limits<u32>::max()));
    }

    void CreateLevelBuffers()
    {
        const u64 sampleCount =
            static_cast<u64>(config_.clipmap.gridResolution) *
            static_cast<u64>(config_.clipmap.gridResolution);
        const u64 bytes = sampleCount *
            sizeof(terrain_stream::TerrainSampleValue);

        for (LevelGpuState& level : levels_)
        {
            level.gpuSampleBuffer = device_.CreateBuffer({
                .sizeBytes = bytes,
                .usage = rhi::BufferUsage::Structured,
                .memory = rhi::MemoryUsage::GpuOnly,
                .initialState = rhi::ResourceState::ShaderResource
            });
        }
    }

    // One small host-visible optics buffer per frame in flight, bound at SRV
    // slot 2 (the pixel stage's g_waterOptics).
    void CreateWaterOpticsBuffers()
    {
        waterOpticsBuffers_.reserve(config_.framesInFlight);
        for (u32 frame = 0; frame < config_.framesInFlight; ++frame)
        {
            waterOpticsBuffers_.push_back(device_.CreateBuffer({
                .sizeBytes = 24U * sizeof(f32),
                .usage = rhi::BufferUsage::Structured,
                .memory = rhi::MemoryUsage::HostVisible,
                .initialState = rhi::ResourceState::ShaderResource
            }));
        }
    }

    void BindWaterOptics(
        rhi::CommandList& commandList,
        const u32 frameIndex,
        const f32 nearPlaneMeters,
        const f32 farPlaneMeters)
    {
        const TerrainWaterOptics& o = waterOptics_;
        const std::array<f32, 24> packed{
            o.absorptionPerMeter.x, o.absorptionPerMeter.y,
            o.absorptionPerMeter.z, o.refractiveIndex,
            o.deepColor.x, o.deepColor.y, o.deepColor.z,
            o.deepColorDepthMeters,
            o.roughness, o.opacity,
            static_cast<f32>(planet_.radiusMeters), 0.0F,
            nearPlaneMeters, farPlaneMeters, 0.0F, 0.0F,
            o.sunDirectionBody.x, o.sunDirectionBody.y,
            o.sunDirectionBody.z, o.sunIrradiance,
            o.skyIrradiance.x, o.skyIrradiance.y, o.skyIrradiance.z, 0.0F
        };

        rhi::Buffer& buffer = *waterOpticsBuffers_[frameIndex];
        std::byte* mapped = buffer.Map();
        std::memcpy(mapped, packed.data(), sizeof(packed));
        buffer.Unmap();
        commandList.SetGraphicsBuffer(1, buffer);
    }

    void CreatePipeline(const shader::Compiler& shaderCompiler)
    {
        const shader::Binary vertexShader = shaderCompiler.Compile({
            .source = kVertexShader,
            .entryPoint = "main",
            .stage = shader::Stage::Vertex,
            .debug = false
        });
        const std::string pixelShaderSource =
            BuildSurfaceEffectPixelShader(
                BuildClipmapBedPixelShader(kPixelShader));
        const shader::Binary pixelShader = shaderCompiler.Compile({
            .source = pixelShaderSource,
            .entryPoint = "main",
            .stage = shader::Stage::Pixel,
            .debug = false
        });

        pipeline_ = device_.CreateGraphicsPipeline({
            .vertexShader = {
                .data = vertexShader.bytecode.data(),
                .size = vertexShader.bytecode.size()
            },
            .pixelShader = {
                .data = pixelShader.bytecode.data(),
                .size = pixelShader.bytecode.size()
            },
            .vertexAttributes = {},
            .vertexStrideBytes = 0,
            .pushConstantDwords = 56,
            .shaderResourceBuffers = 2,
            .topology = rhi::PrimitiveTopology::TriangleList,
            .fillMode = config_.wireframe
                ? rhi::FillMode::Wireframe : rhi::FillMode::Solid,
            .cullMode = rhi::CullMode::None,
            .depthCompare = rhi::DepthCompare::GreaterEqual,
            .depthTest = true,
            .depthWrite = true,
            .colorAttachmentFormats = {
                rhi::TextureFormat::RGBA16_Float,
                rhi::TextureFormat::RGBA16_Float,
                rhi::TextureFormat::RGBA16_Float,
                rhi::TextureFormat::RGBA16_Float
            },
            .colorAttachmentCount = 4U
        });
    }

    void CreateWaterPipeline(const shader::Compiler& shaderCompiler)
    {
        const std::string waterVertexSource =
            BuildClipmapWaterVertexShader(kVertexShader);
        const shader::Binary vertexShader = shaderCompiler.Compile({
            .source = waterVertexSource,
            .entryPoint = "main",
            .stage = shader::Stage::Vertex,
            .debug = false
        });
        const std::string waterPixelSource =
            BuildClipmapWaterPixelShader(waterVertexSource);
        const shader::Binary pixelShader = shaderCompiler.Compile({
            .source = waterPixelSource,
            .entryPoint = "main",
            .stage = shader::Stage::Pixel,
            .debug = false
        });

        waterPipeline_ = device_.CreateGraphicsPipeline({
            .vertexShader = {
                .data = vertexShader.bytecode.data(),
                .size = vertexShader.bytecode.size()
            },
            .pixelShader = {
                .data = pixelShader.bytecode.data(),
                .size = pixelShader.bytecode.size()
            },
            .vertexAttributes = {},
            .vertexStrideBytes = 0,
            .pushConstantDwords = 56,
            .shaderResourceBuffers = 2,
            .sampledTextures = 1,
            .topology = rhi::PrimitiveTopology::TriangleList,
            .fillMode = rhi::FillMode::Solid,
            .cullMode = rhi::CullMode::None,
            .blendMode = rhi::BlendMode::Alpha,
            .depthCompare = rhi::DepthCompare::GreaterEqual,
            .depthTest = true,
            .depthWrite = false,
            .colorAttachmentFormats = {
                rhi::TextureFormat::RGBA16_Float,
                rhi::TextureFormat::RGBA16_Float,
                rhi::TextureFormat::RGBA16_Float,
                rhi::TextureFormat::RGBA16_Float
            },
            .colorAttachmentCount = 1U
        });
    }

    void SetObserverView(const world::WorldPosition& observer)
    {
        const f64 observerRadius = math::Length(observer.meters);
        if (observerRadius <= planet_.radiusMeters)
            throw std::invalid_argument(
                "Orbit terrain preview observer must be above the planet surface.");

        observer_ = observer;
        if (!observerFrameInitialized_)
        {
            observerFrame_ = world::MakeSurfaceFrame(observer.meters);
            observerFrameInitialized_ = true;
        }
        else
        {
            observerFrame_ = world::TransportSurfaceFrameToDirection(
                observerFrame_, observer.meters);
        }
        observerRadiusMeters_ = static_cast<f32>(observerRadius);
    }

    [[nodiscard]] u32 SelectCoverageTier(
        const world::WorldPosition& observer,
        const u32 currentTier) const
    {
        const f64 observerRadiusMeters = math::Length(observer.meters);
        const f64 horizonArcMeters = world::HorizonArcDistanceMeters(
            planet_.radiusMeters, observerRadiusMeters);
        return terrain_view::SelectAdaptiveClipmapTierForHalfExtent(
            baseClipmapConfig_, config_.adaptiveCoverage,
            horizonArcMeters, currentTier);
    }

    [[nodiscard]] CandidateState BuildCandidate(
        const world::WorldPosition& observer)
    {
        const terrain_view::ClipmapConfig candidateConfig =
            terrain_view::ClipmapConfigForTier(
                baseClipmapConfig_, desiredCoverageTier_);
        const bool reuseCommittedState =
            desiredCoverageTier_ == activeCoverageTier_;

        CandidateState candidate{
            .coverageTier = desiredCoverageTier_,
            .clipmapConfig = candidateConfig,
            .layout = terrain_view::BuildClipmapLayout(candidateConfig, observer),
            .tracker = reuseCommittedState
                ? tracker_ : tracker_.Reconfigured(candidateConfig),
            .residency = reuseCommittedState
                ? residency_ : terrain_stream::ToroidalResidency(candidateConfig)
        };

        candidate.motion = candidate.tracker.Update(observer);
        candidate.residencyUpdate = candidate.residency.Apply(candidate.motion);
        terrain_stream::RefreshTerrainMorphRegions(
            candidate.layout, candidate.motion, candidate.residencyUpdate);
        candidate.requests.reserve(levels_.size());

        for (u32 levelIndex = 0;
             levelIndex < static_cast<u32>(levels_.size());
             ++levelIndex)
        {
            const auto& levelUpdate =
                candidate.residencyUpdate.levels[levelIndex];
            if (levelUpdate.refreshRegions.empty())
                continue;

            const auto& level = candidate.layout.levels[levelIndex];
            const bool hasCoarser =
                levelIndex + 1U < static_cast<u32>(levels_.size());
            const terrain_view::ClipmapLevel* coarserLevel = hasCoarser
                ? &candidate.layout.levels[levelIndex + 1U] : nullptr;

            candidate.requests.push_back({
                .levelIndex = levelIndex,
                .resolution = level.gridResolution,
                .spacingMeters = level.sampleSpacingMeters,
                .footprintMeters = level.terrainFootprintMeters,
                .morphToCoarser = hasCoarser,
                .morphStartHalfExtentMeters = level.morphStartHalfExtentMeters,
                .morphEndHalfExtentMeters = level.morphEndHalfExtentMeters,
                .coarseSpacingMeters = coarserLevel != nullptr
                    ? coarserLevel->sampleSpacingMeters : 0.0,
                .coarseFootprintMeters = coarserLevel != nullptr
                    ? coarserLevel->terrainFootprintMeters : 0.0,
                // Shading slope at this ring's own footprint and spacing. It used
                // to be taken at level 0's spacing (1 m) on every ring, which on
                // a coarse ring is a point sample of sub-pixel micro-relief: a
                // vertex sitting on a small crater wall got a steeply tilted
                // normal that interpolated over its six triangles as a
                // hexagonal dark/light blob.
                .fineNormalFootprintMeters = level.terrainFootprintMeters,
                .fineNormalEpsilonMeters = level.sampleSpacingMeters,
                .centerOffsetMeters =
                    candidate.motion.levels[levelIndex].centerOffsetMeters,
                .surfaceFrame =
                    candidate.motion.levels[levelIndex].surfaceFrame,
                .coarseSurfaceFrame = hasCoarser
                    ? candidate.motion.levels[levelIndex + 1U].surfaceFrame
                    : candidate.motion.levels[levelIndex].surfaceFrame,
                .originX = levelUpdate.originX,
                .originY = levelUpdate.originY,
                .regions = levelUpdate.refreshRegions
            });
        }
        return candidate;
    }

    void ResetCommitStats() noexcept
    {
        stats_.generatedSamplesLastUpdate = 0;
        stats_.refreshedRegionsLastUpdate = 0;
        stats_.levelsTouchedLastUpdate = 0;
    }

    void CommitCandidate(CandidateState&& candidate)
    {
        ResetCommitStats();
        const bool coverageTierChanged =
            candidate.coverageTier != activeCoverageTier_;
        const auto fullLevels = static_cast<u32>(std::count_if(
            candidate.residencyUpdate.levels.begin(),
            candidate.residencyUpdate.levels.end(),
            [](const auto& level) { return level.fullRefresh; }));
        if (stats_.committedBatches > 0 && fullLevels > 0)
        {
            ++stats_.rebaseCount;
            stats_.lastRebaseLevels = fullLevels;
            stats_.lastRebaseReason = coverageTierChanged ? "LOD" : "MOVE";
            lastRebaseTime_ = std::chrono::steady_clock::now();
            stats_.secondsSinceLastRebase = 0.0;
        }

        const std::vector<terrain_stream::TerrainSampleRequest> requests =
            std::move(candidate.requests);
        config_.clipmap = candidate.clipmapConfig;
        layout_ = std::move(candidate.layout);
        activeCoverageTier_ = candidate.coverageTier;
        tracker_ = std::move(candidate.tracker);
        residency_ = std::move(candidate.residency);
        motion_ = std::move(candidate.motion);
        residencyUpdate_ = std::move(candidate.residencyUpdate);

        for (const auto& request : requests)
        {
            if (request.regions.empty())
                continue;
            ++stats_.levelsTouchedLastUpdate;
            stats_.refreshedRegionsLastUpdate +=
                static_cast<u32>(request.regions.size());

            u64 requestSampleCount = 0;
            for (const auto& region : request.regions)
                requestSampleCount += static_cast<u64>(region.width) * region.height;
            stats_.generatedSamplesLastUpdate += requestSampleCount;
            RecordDirtyRequest(request);
        }

        stats_.cumulativeGeneratedSamples += stats_.generatedSamplesLastUpdate;
        if (coverageTierChanged)
            ++stats_.coverageTierChanges;
        stats_.adaptiveCoverageTier = activeCoverageTier_;
        stats_.activeBaseSpacingMeters = config_.clipmap.baseSpacingMeters;
        stats_.activeOuterHalfExtentMeters =
            terrain_view::ClipmapOuterHalfExtentMeters(config_.clipmap);
    }

    void InitializeBlocking(const world::WorldPosition& observer)
    {
        ORBIT_PROFILE_SCOPE("terrain.initialize_blocking");
        SetObserverView(observer);
        desiredObserver_ = observer;
        desiredCoverageTier_ = SelectCoverageTier(observer, activeCoverageTier_);
        desiredGeneration_ = 1;
        CandidateState candidate = BuildCandidate(observer);
        CommitCandidate(std::move(candidate));
        committedGeneration_ = desiredGeneration_;
        ++stats_.committedBatches;
        stats_.updatePending = false;
    }

    void ServiceStreaming()
    {
        if (generationFrozen_ || committedGeneration_ == desiredGeneration_)
            return;
        ORBIT_PROFILE_SCOPE("terrain.service_streaming");
        CandidateState candidate = BuildCandidate(desiredObserver_);
        CommitCandidate(std::move(candidate));
        committedGeneration_ = desiredGeneration_;
        ++stats_.committedBatches;
        stats_.updatePending = false;
    }

    void RecordPhysicalPageRefresh()
    {
        if (motion_.levels.size() != levels_.size() ||
            residencyUpdate_.levels.size() != levels_.size())
            return;

        for (u32 levelIndex = 0U;
             levelIndex < static_cast<u32>(levels_.size());
             ++levelIndex)
        {
            const auto& level = layout_.levels[levelIndex];
            const auto& levelUpdate = residencyUpdate_.levels[levelIndex];
            const bool hasCoarser =
                levelIndex + 1U < static_cast<u32>(levels_.size());
            const auto* coarser = hasCoarser
                ? &layout_.levels[levelIndex + 1U] : nullptr;

            terrain_stream::TerrainSampleRequest request{
                .levelIndex = levelIndex,
                .resolution = level.gridResolution,
                .spacingMeters = level.sampleSpacingMeters,
                .footprintMeters = level.terrainFootprintMeters,
                .morphToCoarser = hasCoarser,
                .morphStartHalfExtentMeters = level.morphStartHalfExtentMeters,
                .morphEndHalfExtentMeters = level.morphEndHalfExtentMeters,
                .coarseSpacingMeters = coarser != nullptr
                    ? coarser->sampleSpacingMeters : 0.0,
                .coarseFootprintMeters = coarser != nullptr
                    ? coarser->terrainFootprintMeters : 0.0,
                // Shading slope at this ring's own footprint and spacing. It used
                // to be taken at level 0's spacing (1 m) on every ring, which on
                // a coarse ring is a point sample of sub-pixel micro-relief: a
                // vertex sitting on a small crater wall got a steeply tilted
                // normal that interpolated over its six triangles as a
                // hexagonal dark/light blob.
                .fineNormalFootprintMeters = level.terrainFootprintMeters,
                .fineNormalEpsilonMeters = level.sampleSpacingMeters,
                .centerOffsetMeters =
                    motion_.levels[levelIndex].centerOffsetMeters,
                .surfaceFrame = motion_.levels[levelIndex].surfaceFrame,
                .coarseSurfaceFrame = hasCoarser
                    ? motion_.levels[levelIndex + 1U].surfaceFrame
                    : motion_.levels[levelIndex].surfaceFrame,
                .originX = levelUpdate.originX,
                .originY = levelUpdate.originY,
                .regions = {{
                    .x = 0U,
                    .y = 0U,
                    .width = level.gridResolution,
                    .height = level.gridResolution
                }}
            };
            RecordDirtyRequest(request);
        }
    }

    void RecordDirtyRequest(
        const terrain_stream::TerrainSampleRequest& request)
    {
        if (request.levelIndex >= levels_.size())
            throw std::out_of_range(
                "Orbit terrain sample request references an invalid clipmap level.");

        LevelGpuState& state = levels_[request.levelIndex];
        ++state.currentSerial;
        state.dirtyUpdates.push_back({
            .serial = state.currentSerial,
            .request = request
        });
    }

    [[nodiscard]] u64 PrepareLevelFrame(
        rhi::CommandList& commandList,
        const u32 levelIndex)
    {
        LevelGpuState& state = levels_[levelIndex];
        if (state.appliedSerial == state.currentSerial)
            return 0;

        rhi::Buffer& gpuBuffer = *state.gpuSampleBuffer;
        u64 generatedBytes = 0;
        bool touchedBuffer = false;

        for (const DirtyUpdate& update : state.dirtyUpdates)
        {
            if (update.serial <= state.appliedSerial)
                continue;

            for (const terrain_stream::PhysicalRegion& region :
                 update.request.regions)
            {
                if (!touchedBuffer)
                {
                    commandList.Transition(
                        gpuBuffer,
                        rhi::ResourceState::ShaderResource,
                        rhi::ResourceState::UnorderedAccess);
                    touchedBuffer = true;
                }

                terrain_gpu::GpuFieldRequest gpuRequest{
                    .resolution = update.request.resolution,
                    .spacingMeters = update.request.spacingMeters,
                    .footprintMeters = update.request.footprintMeters,
                    .morphToCoarser = update.request.morphToCoarser,
                    .morphStartHalfExtentMeters =
                        update.request.morphStartHalfExtentMeters,
                    .morphEndHalfExtentMeters =
                        update.request.morphEndHalfExtentMeters,
                    .coarseSpacingMeters = update.request.coarseSpacingMeters,
                    .coarseFootprintMeters = update.request.coarseFootprintMeters,
                    .fineNormalFootprintMeters =
                        update.request.fineNormalFootprintMeters,
                    .fineNormalEpsilonMeters =
                        update.request.fineNormalEpsilonMeters,
                    .centerOffsetMeters = update.request.centerOffsetMeters,
                    .surfaceFrame = update.request.surfaceFrame,
                    .coarseSurfaceFrame = update.request.coarseSurfaceFrame,
                    .originX = update.request.originX,
                    .originY = update.request.originY,
                    .region = {
                        .x = region.x,
                        .y = region.y,
                        .width = region.width,
                        .height = region.height
                    }
                };

                gpuFieldGenerator_.Dispatch(commandList, gpuRequest, gpuBuffer);
                CompositeRegionDelta(
                    commandList, update.request, region, gpuBuffer);

                if (!physicalPages_.empty())
                {
                    commandList.UavBarrier(gpuBuffer);
                    for (const auto& page : physicalPages_)
                    {
                        physicalPageComposite_.Dispatch(
                            commandList,
                            update.request,
                            region,
                            page,
                            planet_.radiusMeters,
                            gpuBuffer);
                        commandList.UavBarrier(gpuBuffer);
                    }
                }

                generatedBytes += static_cast<u64>(region.width) *
                    region.height * sizeof(terrain_stream::TerrainSampleValue);
            }
        }

        if (touchedBuffer)
        {
            commandList.UavBarrier(gpuBuffer);
            commandList.Transition(
                gpuBuffer,
                rhi::ResourceState::UnorderedAccess,
                rhi::ResourceState::ShaderResource);
        }

        state.appliedSerial = state.currentSerial;
        PruneDirtyHistory(state);
        return generatedBytes;
    }

    void CompositeRegionDelta(
        rhi::CommandList& commandList,
        const terrain_stream::TerrainSampleRequest& request,
        const terrain_stream::PhysicalRegion& region,
        rhi::Buffer& gpuBuffer)
    {
        if (regionDeltaComposite_ == nullptr ||
            hydrologyRegionCache_ == nullptr)
            return;

        const terrain_region::DerivedTerrainRegionId regionId =
            hydrologyRegionCache_->IdForDirection(
                world::DirectionAtSurfaceOffset(
                    planet_, request.surfaceFrame, request.centerOffsetMeters));
        const auto hydrologyRegion = hydrologyRegionCache_->TryGet(regionId);
        if (!hydrologyRegion)
            return;

        rhi::Buffer* deltaBuffer = GetOrCreateRegionDeltaBuffer(*hydrologyRegion);
        if (deltaBuffer == nullptr)
            return;

        terrain_gpu::GpuRegionDeltaRequest deltaRequest{};
        deltaRequest.resolution = request.resolution;
        deltaRequest.spacingMeters = request.spacingMeters;
        deltaRequest.surfaceFrame = request.surfaceFrame;
        deltaRequest.centerOffsetMeters = request.centerOffsetMeters;
        deltaRequest.originX = request.originX;
        deltaRequest.originY = request.originY;
        deltaRequest.region = {
            .x = region.x,
            .y = region.y,
            .width = region.width,
            .height = region.height
        };
        deltaRequest.planetRadiusMeters = planet_.radiusMeters;
        deltaRequest.regionSurfaceFrame =
            hydrologyRegion->elevationDelta.surfaceFrame;
        deltaRequest.regionHalfExtentMeters =
            hydrologyRegion->elevationDelta.halfExtentMeters;
        deltaRequest.regionSpacingMeters =
            hydrologyRegion->elevationDelta.spacingMeters;
        deltaRequest.regionResolution = hydrologyRegion->elevationDelta.resolution;
        deltaRequest.edgeFadeStartDot = 0.75F;
        deltaRequest.morphToCoarser = request.morphToCoarser;
        deltaRequest.morphStartHalfExtentMeters = request.morphStartHalfExtentMeters;
        deltaRequest.morphEndHalfExtentMeters = request.morphEndHalfExtentMeters;

        regionDeltaComposite_->Dispatch(
            commandList, deltaRequest, *deltaBuffer, gpuBuffer);
    }

    [[nodiscard]] rhi::Buffer* GetOrCreateRegionDeltaBuffer(
        const terrain_region::DerivedTerrainRegion& hydrologyRegion)
    {
        const auto existing = regionDeltaBuffers_.find(hydrologyRegion.id);
        if (existing != regionDeltaBuffers_.end())
            return existing->second.get();

        const auto& deltas = hydrologyRegion.elevationDelta.elevationDeltaMeters;
        if (deltas.empty())
            return nullptr;

        auto buffer = device_.CreateBuffer({
            .sizeBytes = deltas.size() * sizeof(f32),
            .usage = rhi::BufferUsage::Structured,
            .memory = rhi::MemoryUsage::HostVisible,
            .initialState = rhi::ResourceState::ShaderResource
        });
        std::byte* mapped = buffer->Map();
        std::memcpy(mapped, deltas.data(), deltas.size() * sizeof(f32));
        buffer->Unmap();

        rhi::Buffer* ptr = buffer.get();
        regionDeltaBuffers_.emplace(hydrologyRegion.id, std::move(buffer));
        return ptr;
    }

    static void PruneDirtyHistory(LevelGpuState& state)
    {
        while (!state.dirtyUpdates.empty() &&
               state.dirtyUpdates.front().serial <= state.appliedSerial)
        {
            state.dirtyUpdates.pop_front();
        }
    }

    rhi::Device& device_;
    world::PlanetDefinition planet_;
    terrain_gpu::GpuFieldGenerator& gpuFieldGenerator_;
    terrain_gpu::GpuPhysicalPageComposite physicalPageComposite_;
    std::vector<terrain_gpu::GpuPhysicalSurfacePage> physicalPages_;
    u64 physicalPageGeneration_{0U};
    terrain_gpu::GpuRegionDelta* regionDeltaComposite_{nullptr};
    terrain_region::DerivedTerrainRegionCache* hydrologyRegionCache_{nullptr};
    std::unordered_map<
        terrain_region::DerivedTerrainRegionId,
        std::unique_ptr<rhi::Buffer>,
        terrain_region::DerivedTerrainRegionIdHash>
        regionDeltaBuffers_;

    TerrainPreviewConfig config_;
    SurfaceEffectGpuBinding surfaceEffects_;
    std::unique_ptr<rhi::GraphicsPipeline> waterPipeline_;
    TerrainWaterOptics waterOptics_{};
    std::vector<std::unique_ptr<rhi::Buffer>> waterOpticsBuffers_;
    terrain_view::ClipmapConfig baseClipmapConfig_{};
    terrain_view::ClipmapLayout layout_;
    terrain_view::ClipmapTracker tracker_;
    terrain_stream::ToroidalResidency residency_;
    std::vector<LevelGpuState> levels_;
    std::unique_ptr<rhi::GraphicsPipeline> pipeline_;

    world::WorldPosition observer_{};
    world::WorldPosition desiredObserver_{};
    world::SurfaceFrame observerFrame_{};
    bool observerFrameInitialized_{false};
    terrain_view::ClipmapMotionUpdate motion_;
    terrain_stream::ResidencyUpdate residencyUpdate_;
    f32 observerRadiusMeters_{0.0F};

    bool debugLodColorEnabled_{false};
    bool debugSideCutEnabled_{false};
    bool generationFrozen_{false};
    u64 desiredGeneration_{0};
    u64 committedGeneration_{0};
    u32 activeCoverageTier_{0};
    u32 desiredCoverageTier_{0};
    TerrainStreamingStats stats_{};
    std::chrono::steady_clock::time_point lastRebaseTime_{};
    u32 patchVertexCount_{0};
};

TerrainPreviewRenderer::TerrainPreviewRenderer(
    rhi::Device& device,
    const shader::Compiler& shaderCompiler,
    const world::PlanetDefinition& planet,
    terrain_gpu::GpuFieldGenerator& gpuFieldGenerator,
    const world::WorldPosition& observer,
    TerrainPreviewConfig config,
    terrain_gpu::GpuRegionDelta* regionDeltaComposite,
    terrain_region::DerivedTerrainRegionCache* hydrologyRegionCache)
    : impl_(std::make_unique<Impl>(
        device,
        shaderCompiler,
        planet,
        gpuFieldGenerator,
        observer,
        std::move(config),
        regionDeltaComposite,
        hydrologyRegionCache))
{
}

TerrainPreviewRenderer::~TerrainPreviewRenderer() = default;
TerrainPreviewRenderer::TerrainPreviewRenderer(
    TerrainPreviewRenderer&&) noexcept = default;
TerrainPreviewRenderer& TerrainPreviewRenderer::operator=(
    TerrainPreviewRenderer&&) noexcept = default;

void TerrainPreviewRenderer::UpdateObserver(
    const world::WorldPosition& observer)
{
    impl_->UpdateObserver(observer);
}

void TerrainPreviewRenderer::SetDrySurface(const bool dry) noexcept
{
    impl_->SetDrySurface(dry);
}

void TerrainPreviewRenderer::SetWaterOptics(
    const TerrainWaterOptics& optics) noexcept
{
    impl_->SetWaterOptics(optics);
}

void TerrainPreviewRenderer::DrawWater(
    rhi::CommandList& commandList,
    const u32 frameIndex,
    const u32 targetWidth,
    const u32 targetHeight,
    const TerrainPreviewCamera& camera,
    rhi::Texture& terrainDepth)
{
    impl_->DrawWater(
        commandList, frameIndex, targetWidth, targetHeight, camera, terrainDepth);
}

void TerrainPreviewRenderer::SetDebugVisuals(
    const bool lodColorEnabled,
    const bool sideCutEnabled)
{
    impl_->SetDebugVisuals(lodColorEnabled, sideCutEnabled);
}

void TerrainPreviewRenderer::SetGenerationFrozen(const bool frozen)
{
    impl_->SetGenerationFrozen(frozen);
}

void TerrainPreviewRenderer::SetPhysicalPages(
    const std::span<const terrain_gpu::GpuPhysicalSurfacePage> pages,
    const u64 generation)
{
    impl_->SetPhysicalPages(pages, generation);
}

void TerrainPreviewRenderer::SetSurfaceEffects(
    const std::span<const SurfaceEffectGpuStamp> effects)
{
    impl_->SetSurfaceEffects(effects);
}

void TerrainPreviewRenderer::Draw(
    rhi::CommandList& commandList,
    const u32 frameIndex,
    const u32 targetWidth,
    const u32 targetHeight,
    const TerrainPreviewCamera& camera)
{
    impl_->Draw(commandList, frameIndex, targetWidth, targetHeight, camera);
}

u32 TerrainPreviewRenderer::VertexCount() const noexcept
{
    return impl_->VertexCount();
}

u32 TerrainPreviewRenderer::IndexCount() const noexcept
{
    return impl_->IndexCount();
}

const TerrainStreamingStats& TerrainPreviewRenderer::StreamingStats() const noexcept
{
    return impl_->StreamingStats();
}
} // namespace orbit::terrain_render

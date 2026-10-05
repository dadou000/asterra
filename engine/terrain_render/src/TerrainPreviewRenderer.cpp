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
    const bool debugSampleHealthEnabled,
    const bool debugHoleViewEnabled,
    const bool debugProjectionViewEnabled,
    const bool debugSideCutEnabled,
    const bool drySurface,
    const f32 seaLevelMeters,
    const f32 selfFade,
    const f32 finerFade,
    const f32 bandZoneFraction) noexcept
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
    store(37, debugProjectionViewEnabled ? 4.0F : debugHoleViewEnabled ? 3.0F : (debugSampleHealthEnabled ? 2.0F : (debugLodColorEnabled ? 1.0F : 0.0F)));
    store(38, debugSideCutEnabled ? 1.0F : 0.0F);
    store(39, static_cast<f32>(innerHoleCenterOffset.y));

    store(40, static_cast<f32>(motion.centerOffsetMeters.x));
    store(41, static_cast<f32>(motion.centerOffsetMeters.y));
    store(42, drySurface ? 1.0F : 0.0F);
    store(43, seaLevelMeters);

    store(44, static_cast<f32>(observerFrame.east.x));
    store(45, static_cast<f32>(observerFrame.east.y));
    store(46, static_cast<f32>(observerFrame.east.z));
    store(47, selfFade);

    store(48, static_cast<f32>(observerFrame.up.x));
    store(49, static_cast<f32>(observerFrame.up.y));
    store(50, static_cast<f32>(observerFrame.up.z));
    store(51, finerFade);

    // < 0: ladder (selfFade/finerFade are time fades). >= 0: distance bands
    // (selfFade/finerFade are the band's inner/outer edge in metres).
    store(55, bandZoneFraction);

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
          levels_(terrain_view::ClipmapLevelCount(config_.clipmap))
    {
        if (math::Length(observer.meters) <= planet_.radiusMeters)
            throw std::invalid_argument(
                "Orbit terrain preview observer must be above the planet surface.");
        if (config_.framesInFlight == 0)
            throw std::invalid_argument(
                "Orbit terrain preview requires at least one frame in flight.");

        planner_.SetConfig(config_.planner);
        desiredPlan_.dynamic = false;
        desiredPlan_.firstLevel = 0U;
        desiredPlan_.lastLevel =
            terrain_view::ClipmapLevelCount(config_.clipmap) - 1U;
        plan_ = desiredPlan_;
        levelValid_.assign(terrain_view::ClipmapLevelCount(config_.clipmap), 0U);

        groundReadbackPending_.assign(config_.framesInFlight, {});
        for (u32 slot = 0U; slot < config_.framesInFlight; ++slot)
        {
            groundReadback_.push_back(device_.CreateBuffer({
                .sizeBytes = 4U * sizeof(f32),
                .usage = rhi::BufferUsage::Generic,
                .memory = rhi::MemoryUsage::HostVisible,
                .initialState = rhi::ResourceState::CopyDestination}));
        }

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
        liveObserver_ = observer;
        // Frozen: the clipmap stays where it was and the camera is free to leave it.
        AdvanceLiveFrame(observer);
        if (clipmapFrozen_)
            return;
        SetObserverView(observer);
        desiredObserver_ = observer;
        desiredCoverageTier_ = SelectCoverageTier(observer, desiredCoverageTier_);
        ++desiredGeneration_;
        ServiceStreaming();
    }

    void SetDebugVisuals(
        const bool lodColorEnabled,
        const bool sideCutEnabled,
        const bool sampleHealthEnabled,
        const bool holeViewEnabled,
        const bool projectionViewEnabled) noexcept
    {
        debugLodColorEnabled_ = lodColorEnabled;
        debugSideCutEnabled_ = sideCutEnabled;
        debugSampleHealthEnabled_ = sampleHealthEnabled;
        debugHoleViewEnabled_ = holeViewEnabled;
        debugProjectionViewEnabled_ = projectionViewEnabled;
    }

    void SetGenerationFrozen(const bool frozen) noexcept
    {
        generationFrozen_ = frozen;
    }

    void SetWireframe(const bool wireframe) noexcept
    {
        wireframe_ = wireframe;
    [[nodiscard]] const world::SurfaceFrame& CameraFrame() const noexcept
    {
        return liveFrame_;
    }

    }

    // Freezes the whole clipmap (plan, window position, residency, content) at
    // the current observer. The camera keeps moving; the terrain is drawn from
    // wherever it is relative to the frozen window. Unfreezing snaps the window
    // back to the camera and re-plans.
    void SetClipmapFrozen(const bool frozen)
    {
        if (frozen == clipmapFrozen_)
            return;
        clipmapFrozen_ = frozen;
        if (frozen)
        {
            // The frame the frozen geometry was authored in (transported, not rebuilt).
            frozenCameraFrame_ = observerFrame_;
            return;
        }
        UpdateObserver(liveObserver_);
        planDirty_ = true;
    }

    [[nodiscard]] bool ClipmapFrozen() const noexcept
    {
        return clipmapFrozen_;
    }
    void SetClipmapPlanner(const terrain_view::ClipmapPlannerConfig& config)
    {
        // Callers may push the setting every frame; only a real change resets
        // the planner (and its hysteresis).
        if (config == config_.planner)
            return;
        config_.planner = config;
        planner_.SetConfig(config);
        // Re-plan on the next Draw even if the camera has not moved.
        planDirty_ = true;
    }

    void SetGroundElevationHint(const f64 elevationMeters) noexcept
    {
        if (std::isfinite(elevationMeters))
            groundElevationHintMeters_ = elevationMeters;
    }

    [[nodiscard]] const terrain_view::ClipmapPlan& ClipmapPlan() const noexcept
    {
        return plan_;
    }

    [[nodiscard]] std::vector<TerrainClipmapLevelSummary> ClipmapLevels() const
    {
        std::vector<TerrainClipmapLevelSummary> result;
        result.reserve(layout_.levels.size());
        for (const terrain_view::ClipmapLevel& level : layout_.levels)
        {
            const terrain_view::ClipmapBand band =
                terrain_view::ClipmapLevelBand(baseClipmapConfig_, level.index);
            result.push_back({
                .level = level.index,
                .active = level.active,
                .spacingMeters = level.sampleSpacingMeters,
                .halfExtentMeters = level.outerHalfExtentMeters,
                .bandInnerMeters = band.innerMeters,
                .bandOuterMeters = band.outerMeters,
                .gridResolution = level.gridResolution,
                .drawnVertices = level.index < patchVertexCounts_.size()
                    ? patchVertexCounts_[level.index]
                    : 0U});
        }
        return result;
    }

    [[nodiscard]] bool ClipmapBanded() const noexcept
    {
        return baseClipmapConfig_.Banded();
    }

    [[nodiscard]] TerrainRenderedGround RenderedGround() const noexcept
    {
        return renderedGround_;
    }

    // Reads back the ground heights recorded for this frame slot the last time it
    // was used (the slot's earlier submission has retired by now).
    void CollectGroundReadback(const u32 frameIndex)
    {
        if (frameIndex >= groundReadback_.size() ||
            !groundReadbackPending_[frameIndex].valid)
            return;
        GroundReadbackRequest& request = groundReadbackPending_[frameIndex];
        request.valid = false;
        const std::byte* mapped = groundReadback_[frameIndex]->Map();
        f32 corners[4]{};
        std::memcpy(corners, mapped, sizeof(corners));
        groundReadback_[frameIndex]->Unmap();
        f32 highest = corners[0];
        for (const f32 corner : corners)
            highest = std::max(highest, corner);
        if (!std::isfinite(highest))
            return;
        renderedGround_ = {
            .valid = true,
            .elevationMeters = static_cast<f64>(highest),
            .direction = request.direction,
            .spacingMeters = request.spacingMeters,
            .cornerDirections = request.cornerDirections,
            .cornerElevations = {corners[0], corners[1], corners[2], corners[3]},
            .footprintMeters = request.footprintMeters};
    }

    // Copies the four vertex elevations around the point below the camera out of
    // the finest drawn level. Called right after that level is prepared.
    // True once a readback is recorded (or already waiting) for this frame slot.
    bool RecordGroundReadback(
        rhi::CommandList& commandList,
        const u32 frameIndex,
        const u32 levelIndex)
    {
        if (frameIndex >= groundReadback_.size())
            return true;
        if (groundReadbackPending_[frameIndex].valid)
            return true;
        if (levelIndex >= motion_.levels.size() ||
            levelIndex >= residencyUpdate_.levels.size() ||
            levelValid_[levelIndex] == 0U)
            return false;
        const terrain_view::ClipmapLevel& level = layout_.levels[levelIndex];
        const terrain_view::ClipmapLevelMotion& motion = motion_.levels[levelIndex];
        const math::Double3 direction = math::Normalize(liveObserver_.meters);
        const math::Double2 offset = world::SurfaceOffsetBetweenDirections(
            planet_, motion.surfaceFrame, direction);
        const f64 spacing = level.sampleSpacingMeters;
        const f64 half = (static_cast<f64>(level.gridResolution) - 1.0) * 0.5;
        const f64 logicalX = (offset.x - motion.centerOffsetMeters.x) / spacing + half;
        const f64 logicalY = (offset.y - motion.centerOffsetMeters.y) / spacing + half;
        const i64 maxIndex = static_cast<i64>(level.gridResolution) - 1;
        if (!(logicalX >= 0.0 && logicalY >= 0.0 &&
              logicalX <= static_cast<f64>(maxIndex) &&
              logicalY <= static_cast<f64>(maxIndex)))
            return false; // the camera is outside this level's window
        const i64 baseX = static_cast<i64>(std::floor(logicalX));
        const i64 baseY = static_cast<i64>(std::floor(logicalY));
        const terrain_stream::LevelResidencyUpdate& residency =
            residencyUpdate_.levels[levelIndex];
        rhi::Buffer& source = *levels_[levelIndex].gpuSampleBuffer;
        commandList.Transition(
            source,
            rhi::ResourceState::ShaderResource,
            rhi::ResourceState::CopySource);
        std::array<math::Double3, 4> cornerDirections{};
        for (u32 corner = 0U; corner < 4U; ++corner)
        {
            const i64 x = std::clamp<i64>(baseX + (corner & 1U), 0, maxIndex);
            const i64 y = std::clamp<i64>(baseY + (corner >> 1U), 0, maxIndex);
            cornerDirections[corner] = world::DirectionAtSurfaceOffset(
                planet_,
                motion.surfaceFrame,
                math::Double2{
                    motion.centerOffsetMeters.x +
                        (static_cast<f64>(x) - half) * spacing,
                    motion.centerOffsetMeters.y +
                        (static_cast<f64>(y) - half) * spacing});
            const u64 resolution = level.gridResolution;
            const u64 physicalX = (static_cast<u64>(x) + residency.originX) % resolution;
            const u64 physicalY = (static_cast<u64>(y) + residency.originY) % resolution;
            commandList.CopyBuffer(
                source,
                (physicalY * resolution + physicalX) * sizeof(terrain_stream::TerrainSampleValue),
                *groundReadback_[frameIndex],
                static_cast<u64>(corner) * sizeof(f32),
                sizeof(f32));
        }
        commandList.Transition(
            source,
            rhi::ResourceState::CopySource,
            rhi::ResourceState::ShaderResource);
        groundReadbackPending_[frameIndex] = {
            .valid = true,
            .direction = direction,
            .spacingMeters = spacing,
            .footprintMeters = level.terrainFootprintMeters,
            .cornerDirections = cornerDirections};
        return true;
    }

    // Plans the active clipmap range for this camera. A change queues a rebuild
    // of the candidate layout (ServiceStreaming applies it right after).
    void UpdatePlan(
        const TerrainPreviewCamera& camera,
        const u32 targetWidth,
        const u32 targetHeight)
    {
        if (clipmapFrozen_)
        {
            // The plan holds, but a dissolve already under way finishes.
            if (!levelFades_.empty() && !baseClipmapConfig_.Banded())
                AdvanceLevelFades();
            return;
        }
        terrain_view::ClipmapPlanView view;
        // The camera vectors are in the observer's local frame (x east, y up,
        // z north), so the observer sits on the local up axis at its radius.
        view.position = {0.0, math::Length(observer_.meters), 0.0};
        view.forward = {
            static_cast<f64>(camera.forward.x),
            static_cast<f64>(camera.forward.y),
            static_cast<f64>(camera.forward.z)};
        view.up = {
            static_cast<f64>(camera.up.x),
            static_cast<f64>(camera.up.y),
            static_cast<f64>(camera.up.z)};
        view.verticalFovRadians = camera.verticalFovRadians > 0.0F
            ? static_cast<f64>(camera.verticalFovRadians)
            : static_cast<f64>(config_.verticalFovRadians);
        view.viewportWidthPixels = targetWidth;
        view.viewportHeightPixels = targetHeight;
        view.planetRadiusMeters = planet_.radiusMeters;
        view.groundElevationMeters = groundElevationHintMeters_;

        const terrain_view::ClipmapPlan planned =
            planner_.Plan(baseClipmapConfig_, view);
        stats_.nearestGroundMeters = planned.nearestGroundMeters;
        stats_.visibleArcMeters = planned.visibleArcMeters;
        stats_.requiredSpacingMeters = planned.requiredSpacingMeters;

        // The planner's range is the target. Levels fade toward it, so the range
        // the layout (generation, drawing) uses also holds the levels still
        // fading out.
        plannedPlan_ = planned;
        terrain_view::ClipmapPlan next = planned;
        // Banded levels need no time fade: their coverage already follows the
        // camera's distance continuously.
        if (!baseClipmapConfig_.Banded())
        {
            AdvanceLevelFades();
            for (u32 level = 0U; level < static_cast<u32>(levelFades_.size()); ++level)
            {
                if (planned.Active(level) || levelFades_[level] <= 0.0F)
                    continue;
                next.firstLevel = std::min(next.firstLevel, level);
                next.lastLevel = std::max(next.lastLevel, level);
            }
        }

        if (!(next == desiredPlan_) || planDirty_)
        {
            desiredPlan_ = next;
            planDirty_ = false;
            // A new candidate must be built even though the observer is the same.
            ++desiredGeneration_;
            ++stats_.planChanges;
        }
    }

    // How drawn a level is, 0..1. A level the plan adds fades in, one it drops
    // fades out (the fragment shader dithers it against its neighbour), so
    // levels dissolve instead of popping.
    // Distance-band constants for the draw (experimental banded clipmap); in the
    // ladder the fallback is the time fade.
    [[nodiscard]] f32 BandInner(const u32 level, const f32 fallback) const noexcept
    {
        return baseClipmapConfig_.Banded()
            ? static_cast<f32>(
                  terrain_view::ClipmapLevelBand(baseClipmapConfig_, level).innerMeters)
            : fallback;
    }
    [[nodiscard]] f32 BandOuter(const u32 level, const f32 fallback) const noexcept
    {
        return baseClipmapConfig_.Banded()
            ? static_cast<f32>(
                  terrain_view::ClipmapLevelBand(baseClipmapConfig_, level).outerMeters)
            : fallback;
    }
    [[nodiscard]] f32 BandZone() const noexcept
    {
        return baseClipmapConfig_.Banded()
            ? static_cast<f32>(baseClipmapConfig_.bandZoneFraction)
            : -1.0F;
    }

    void SetLevelFadeSeconds(const f64 seconds) noexcept
    {
        if (std::isfinite(seconds))
            levelFadeSeconds_ = std::clamp(seconds, 0.0, 5.0);
    }

    [[nodiscard]] f32 LevelFade(const u32 level) const noexcept
    {
        return level < levelFades_.size() ? levelFades_[level] : 1.0F;
    }

    void AdvanceLevelFades()
    {
        const u32 ladder = terrain_view::ClipmapLevelCount(baseClipmapConfig_);
        const auto now = std::chrono::steady_clock::now();
        const f64 elapsed = std::chrono::duration<f64>(now - lastFadeTime_).count();
        lastFadeTime_ = now;

        const bool first = levelFades_.size() != ladder;
        if (first)
            levelFades_.assign(ladder, 0.0F);
        // A long frame (a stall) must not skip the dissolve entirely.
        const f32 step = levelFadeSeconds_ > 0.0
            ? static_cast<f32>(std::clamp(elapsed, 0.0, 0.05) / levelFadeSeconds_)
            : 1.0F;

        for (u32 level = 0U; level < ladder; ++level)
        {
            const bool wanted = plannedPlan_.Active(level);
            if (first)
                levelFades_[level] = wanted ? 1.0F : 0.0F;
            else
                levelFades_[level] = std::clamp(
                    levelFades_[level] + (wanted ? step : -step), 0.0F, 1.0F);
        }
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

        math::Float3 eye{0.0F, 0.0F, 0.0F};
        if (clipmapFrozen_)
        {
            // The geometry is in the frozen observer's local frame. Put the live
            // camera where it really is relative to that frame.
            const world::SurfaceFrame& liveFrame = liveFrame_;
            const auto fromLive = [&liveFrame](const math::Float3& v)
            {
                return liveFrame.east * static_cast<f64>(v.x) +
                    liveFrame.up * static_cast<f64>(v.y) +
                    liveFrame.north * static_cast<f64>(v.z);
            };
            cameraForward = ToObserverLocal(
                fromLive(cameraForward), frozenCameraFrame_);
            cameraUp = ToObserverLocal(
                fromLive(cameraUp), frozenCameraFrame_);
            eye = ToObserverLocal(
                liveObserver_.meters - observer_.meters, frozenCameraFrame_);
        }

        const math::Mat4 view = math::LookAtLH(
            eye, eye + cameraForward, cameraUp);
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
        // A wireframe view shows the mesh, not a water surface over it.
        if (targetWidth == 0 || targetHeight == 0 || waterOptics_.opacity <= 0.0F ||
            wireframe_)
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
            if (!layout_.levels[levelIndex].active)
                continue;
            const terrain_view::ClipmapLevel& level =
                layout_.levels[levelIndex];
            const terrain_view::ClipmapLevel* coarserLevel =
                !baseClipmapConfig_.Banded() &&
                levelIndex + 1U < static_cast<u32>(levels_.size())
                    ? &layout_.levels[levelIndex + 1U]
                    : nullptr;
            // The finest active level has no finer level to leave a hole for.
            const terrain_view::ClipmapLevelMotion* finerMotion =
                levelIndex > 0U && layout_.levels[levelIndex - 1U].active
                    ? &motion_.levels[levelIndex - 1U]
                    : nullptr;

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
                false,
                false,
                false,
                config_.drySurface,
                waterOptics_.seaLevelMeters,
                BandInner(levelIndex, 1.0F),
                BandOuter(levelIndex, 1.0F),
                BandZone());

            commandList.SetGraphicsConstants(constants);
            commandList.SetGraphicsBuffer(
                0, *levels_[levelIndex].gpuSampleBuffer);
            commandList.Draw(patchVertexCounts_[levelIndex]);
        }
    }

    void Draw(
        rhi::CommandList& commandList,
        const u32 frameIndex,
        const u32 targetWidth,
        const u32 targetHeight,
        const TerrainPreviewCamera& camera)
    {
        UpdatePlan(camera, targetWidth, targetHeight);
        ServiceStreaming();
        RefreshForNewRegions();
        if (frameIndex < config_.framesInFlight)
            CollectGroundReadback(frameIndex);
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
        if (wireframe_ != config_.wireframe)
        {
            if (!alternatePipeline_)
                alternatePipeline_ = CreateTerrainPipeline(wireframe_);
            commandList.SetGraphicsPipeline(*alternatePipeline_);
        }
        else
        {
            commandList.SetGraphicsPipeline(*pipeline_);
        }
        surfaceEffects_.Bind(commandList, frameIndex);

        bool groundRecordedThisFrame = false;
        for (u32 levelIndex = 0;
             levelIndex < static_cast<u32>(levels_.size());
             ++levelIndex)
        {
            if (!layout_.levels[levelIndex].active)
                continue;
            stats_.uploadedBytesLastFrame +=
                PrepareLevelFrame(commandList, levelIndex);
            // The finest drawn level that holds the camera feeds the ground height.
            if (!groundRecordedThisFrame)
                groundRecordedThisFrame =
                    RecordGroundReadback(commandList, frameIndex, levelIndex);

            const terrain_view::ClipmapLevel& level =
                layout_.levels[levelIndex];
            const terrain_view::ClipmapLevel* coarserLevel =
                !baseClipmapConfig_.Banded() &&
                levelIndex + 1U < static_cast<u32>(levels_.size())
                    ? &layout_.levels[levelIndex + 1U]
                    : nullptr;
            // The finest active level has no finer level to leave a hole for.
            const terrain_view::ClipmapLevelMotion* finerMotion =
                levelIndex > 0U && layout_.levels[levelIndex - 1U].active
                    ? &motion_.levels[levelIndex - 1U]
                    : nullptr;

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
                debugSampleHealthEnabled_,
                debugHoleViewEnabled_,
                debugProjectionViewEnabled_,
                debugSideCutEnabled_,
                config_.drySurface,
                waterOptics_.seaLevelMeters,
                BandInner(levelIndex, LevelFade(levelIndex)),
                BandOuter(
                    levelIndex,
                    finerMotion != nullptr ? LevelFade(levelIndex - 1U) : 1.0F),
                BandZone());

            commandList.SetGraphicsConstants(constants);
            commandList.SetGraphicsBuffer(
                0, *levels_[levelIndex].gpuSampleBuffer);
            commandList.Draw(patchVertexCounts_[levelIndex]);
            ++stats_.drawCallsLastFrame;
        }

        stats_.cumulativeUploadedBytes += stats_.uploadedBytesLastFrame;
    }

    [[nodiscard]] u32 VertexCount() const noexcept
    {
        u64 total = 0;
        for (const u32 count : patchVertexCounts_)
            total += count;
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
        terrain_view::ClipmapPlan plan;
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
        // One vertex count per level: coarse levels can carry a denser grid.
        patchVertexCounts_.clear();
        for (u32 level = 0; level < terrain_view::ClipmapLevelCount(config_.clipmap); ++level)
        {
            const u64 cellsPerAxis = static_cast<u64>(
                terrain_view::ClipmapLevelGridResolution(config_.clipmap, level)) - 1ULL;
            const u64 vertexCount = cellsPerAxis * cellsPerAxis * 6ULL;
            patchVertexCounts_.push_back(static_cast<u32>(std::min<u64>(
                vertexCount, std::numeric_limits<u32>::max())));
        }
    }

    void CreateLevelBuffers()
    {
        for (u32 levelIndex = 0; levelIndex < levels_.size(); ++levelIndex)
        {
            LevelGpuState& level = levels_[levelIndex];
            const u64 resolution = terrain_view::ClipmapLevelGridResolution(
                config_.clipmap, levelIndex);
            const u64 bytes = resolution * resolution *
                sizeof(terrain_stream::TerrainSampleValue);
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
        terrainVertexShader_ = shaderCompiler.Compile({
            .source = kVertexShader,
            .entryPoint = "main",
            .stage = shader::Stage::Vertex,
            .debug = false
        });
        const std::string pixelShaderSource =
            BuildSurfaceEffectPixelShader(
                BuildClipmapBedPixelShader(kPixelShader));
        terrainPixelShader_ = shaderCompiler.Compile({
            .source = pixelShaderSource,
            .entryPoint = "main",
            .stage = shader::Stage::Pixel,
            .debug = false
        });

        pipeline_ = CreateTerrainPipeline(config_.wireframe);
    }

    // The terrain pipeline in a fill mode. The wireframe variant is built the
    // first time it is drawn, from the already compiled shaders.
    [[nodiscard]] std::unique_ptr<rhi::GraphicsPipeline>
    CreateTerrainPipeline(const bool wireframe)
    {
        const shader::Binary& vertexShader = terrainVertexShader_;
        const shader::Binary& pixelShader = terrainPixelShader_;

        return device_.CreateGraphicsPipeline({
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
            .fillMode = wireframe
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
    // Frame the Studio camera vectors are expressed in: the live observer's
    // transported frame. It advances with the live observer even while the
    // clipmap is frozen, and equals observerFrame_ otherwise.
    void AdvanceLiveFrame(const world::WorldPosition& observer)
    {
        if (!liveFrameInitialized_)
        {
            liveFrame_ = world::MakeSurfaceFrame(observer.meters);
            liveFrameInitialized_ = true;
        }
        else
        {
            liveFrame_ = world::TransportSurfaceFrameToDirection(
                liveFrame_, observer.meters);
        }
    }

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
            .plan = desiredPlan_,
            .coverageTier = desiredCoverageTier_,
            .clipmapConfig = candidateConfig,
            .layout = terrain_view::BuildClipmapLayout(candidateConfig, observer),
            .tracker = reuseCommittedState
                ? tracker_ : tracker_.Reconfigured(candidateConfig),
            .residency = reuseCommittedState
                ? residency_ : terrain_stream::ToroidalResidency(candidateConfig)
        };
        // A tier change rebuilds every level, so nothing is valid afterwards.
        if (!reuseCommittedState)
            std::fill(levelValid_.begin(), levelValid_.end(), u8{0});
        terrain_view::ApplyClipmapActiveRange(
            candidate.layout,
            candidate.plan.firstLevel,
            candidate.plan.lastLevel);

        candidate.motion = candidate.tracker.Update(observer);
        candidate.residencyUpdate = candidate.residency.Apply(candidate.motion);
        // Banded levels do not geomorph into a parent.
        if (!candidateConfig.Banded())
        {
            terrain_stream::RefreshTerrainMorphRegions(
                candidate.layout, candidate.motion, candidate.residencyUpdate);
        }
        candidate.requests.reserve(levels_.size());

        for (u32 levelIndex = 0;
             levelIndex < static_cast<u32>(levels_.size());
             ++levelIndex)
        {
            const auto& levelUpdate =
                candidate.residencyUpdate.levels[levelIndex];
            const auto& level = candidate.layout.levels[levelIndex];
            // Levels outside the plan are neither drawn nor generated.
            if (!level.active)
                continue;
            // A level that has just become active (or was never generated) holds
            // stale samples: regenerate it in full once.
            const bool needsFull =
                levelValid_[levelIndex] == 0U && !levelUpdate.fullRefresh;
            if (levelUpdate.refreshRegions.empty() && !needsFull)
                continue;

            const bool hasCoarser = !candidateConfig.Banded() &&
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
                .regions = needsFull
                    ? std::vector<terrain_stream::PhysicalRegion>{{
                          .x = 0U,
                          .y = 0U,
                          .width = level.gridResolution,
                          .height = level.gridResolution}}
                    : levelUpdate.refreshRegions
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
        plan_ = candidate.plan;
        stats_.plannerDynamic = plan_.dynamic;
        stats_.ladderLevels = static_cast<u32>(levels_.size());
        stats_.activeFirstLevel = plan_.firstLevel;
        stats_.activeLastLevel = plan_.lastLevel;
        layout_ = std::move(candidate.layout);
        activeCoverageTier_ = candidate.coverageTier;
        tracker_ = std::move(candidate.tracker);
        residency_ = std::move(candidate.residency);
        motion_ = std::move(candidate.motion);
        residencyUpdate_ = std::move(candidate.residencyUpdate);

        // Inactive levels keep no pending work; active ones are valid from here.
        for (u32 levelIndex = 0U; levelIndex < static_cast<u32>(levels_.size());
             ++levelIndex)
        {
            if (layout_.levels[levelIndex].active)
            {
                levelValid_[levelIndex] = 1U;
                continue;
            }
            levelValid_[levelIndex] = 0U;
            LevelGpuState& idle = levels_[levelIndex];
            idle.dirtyUpdates.clear();
            idle.appliedSerial = idle.currentSerial;
        }

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
        liveObserver_ = observer;
        wireframe_ = config_.wireframe;
        SetObserverView(observer);
        desiredObserver_ = observer;
        desiredCoverageTier_ = SelectCoverageTier(observer, activeCoverageTier_);
        desiredGeneration_ = 1;
        AdvanceLiveFrame(observer);
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

    // Hydrology region deltas arrive asynchronously and are composited into a
    // sample only when it is generated. A level that scrolls by strips keeps the
    // samples it already has, so one generated before a region was ready would
    // stay without it (two levels then disagree about the terrain). When the
    // cache's content changes, regenerate the active levels once. Throttled: the
    // revision can move several times in a burst.
    void RefreshForNewRegions()
    {
        if (hydrologyRegionCache_ == nullptr || regionDeltaComposite_ == nullptr)
            return;
        const u64 revision = hydrologyRegionCache_->ContentRevision();
        const auto now = std::chrono::steady_clock::now();
        if (revision != regionRevision_)
        {
            regionRevision_ = revision;
            regionRefreshPending_ = true;
        }
        if (!regionRefreshPending_ ||
            std::chrono::duration<f64>(now - lastRegionRefresh_).count() < 0.5)
            return;
        regionRefreshPending_ = false;
        lastRegionRefresh_ = now;
        RecordPhysicalPageRefresh();
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
            if (!level.active)
            {
                levelValid_[levelIndex] = 0U;
                continue;
            }
            const auto& levelUpdate = residencyUpdate_.levels[levelIndex];
            const bool hasCoarser = !baseClipmapConfig_.Banded() &&
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
    // The other fill mode, built the first time it is drawn.
    std::unique_ptr<rhi::GraphicsPipeline> alternatePipeline_;
    shader::Binary terrainVertexShader_;
    shader::Binary terrainPixelShader_;

    // The observer the clipmap is built around (frozen while clipmapFrozen_)
    // and where the camera really is.
    world::WorldPosition liveObserver_{};
    world::SurfaceFrame frozenCameraFrame_{};
    bool wireframe_{false};
    bool clipmapFrozen_{false};
    world::WorldPosition observer_{};
    world::WorldPosition desiredObserver_{};
    world::SurfaceFrame liveFrame_{};
    bool liveFrameInitialized_{false};
    world::SurfaceFrame observerFrame_{};
    bool observerFrameInitialized_{false};
    terrain_view::ClipmapMotionUpdate motion_;
    terrain_stream::ResidencyUpdate residencyUpdate_;
    f32 observerRadiusMeters_{0.0F};

    bool debugLodColorEnabled_{false};
    bool debugSampleHealthEnabled_{false};
    bool debugHoleViewEnabled_{false};
    bool debugProjectionViewEnabled_{false};
    bool debugSideCutEnabled_{false};
    bool generationFrozen_{false};
    u64 desiredGeneration_{0};
    u64 committedGeneration_{0};
    u32 activeCoverageTier_{0};
    u32 desiredCoverageTier_{0};
    terrain_view::ClipmapPlanner planner_;
    terrain_view::ClipmapPlan plan_;
    terrain_view::ClipmapPlan desiredPlan_;
    // What the planner asked for (the layout's range also holds fading levels).
    terrain_view::ClipmapPlan plannedPlan_;
    std::vector<f32> levelFades_;
    f64 levelFadeSeconds_{0.4};
    struct GroundReadbackRequest
    {
        bool valid{false};
        math::Double3 direction{};
        f64 spacingMeters{0.0};
        f64 footprintMeters{0.0};
        std::array<math::Double3, 4> cornerDirections{};
    };
    std::vector<std::unique_ptr<rhi::Buffer>> groundReadback_;
    std::vector<GroundReadbackRequest> groundReadbackPending_;
    TerrainRenderedGround renderedGround_{};
    u64 regionRevision_{0};
    bool regionRefreshPending_{false};
    std::chrono::steady_clock::time_point lastRegionRefresh_{};
    std::chrono::steady_clock::time_point lastFadeTime_{};
    bool planDirty_{false};
    f64 groundElevationHintMeters_{0.0};
    // Per level: 1 when its sample buffer holds current data for the layout.
    std::vector<u8> levelValid_;
    TerrainStreamingStats stats_{};
    std::chrono::steady_clock::time_point lastRebaseTime_{};
    std::vector<u32> patchVertexCounts_;
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
const world::SurfaceFrame& TerrainPreviewRenderer::CameraFrame() const noexcept
{
    return impl_->CameraFrame();
}

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
    const bool sideCutEnabled,
    const bool sampleHealthEnabled,
    const bool holeViewEnabled,
    const bool projectionViewEnabled)
{
    impl_->SetDebugVisuals(lodColorEnabled, sideCutEnabled, sampleHealthEnabled, holeViewEnabled, projectionViewEnabled);
}

void TerrainPreviewRenderer::SetGenerationFrozen(const bool frozen)
{
    impl_->SetGenerationFrozen(frozen);
}

void TerrainPreviewRenderer::SetWireframe(const bool wireframe)
{
    impl_->SetWireframe(wireframe);
}

void TerrainPreviewRenderer::SetClipmapFrozen(const bool frozen)
{
    impl_->SetClipmapFrozen(frozen);
}

bool TerrainPreviewRenderer::ClipmapFrozen() const noexcept
{
    return impl_->ClipmapFrozen();
}

void TerrainPreviewRenderer::SetClipmapPlanner(
    const terrain_view::ClipmapPlannerConfig& config)
{
    impl_->SetClipmapPlanner(config);
}

void TerrainPreviewRenderer::SetLevelFadeSeconds(const f64 seconds) noexcept
{
    impl_->SetLevelFadeSeconds(seconds);
}

void TerrainPreviewRenderer::SetGroundElevationHint(
    const f64 elevationMeters) noexcept
{
    impl_->SetGroundElevationHint(elevationMeters);
}

std::vector<TerrainClipmapLevelSummary> TerrainPreviewRenderer::ClipmapLevels()
    const
{
    return impl_->ClipmapLevels();
}

bool TerrainPreviewRenderer::ClipmapBanded() const noexcept
{
    return impl_->ClipmapBanded();
}

TerrainRenderedGround TerrainPreviewRenderer::RenderedGround() const noexcept
{
    return impl_->RenderedGround();
}

const terrain_view::ClipmapPlan& TerrainPreviewRenderer::ClipmapPlan()
    const noexcept
{
    return impl_->ClipmapPlan();
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

#pragma once

#include <orbit/render_graph/RenderGraph.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/volume_fields/VolumeFieldStorage.hpp>
#include <orbit/world_model/VolumeSchemas.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace orbit::volume_solver
{
enum class SurfaceVolumeDebugView : u8
{
    Off = 0U,
    Density = 1U,
    Velocity = 2U,
    FieldSlice = 3U
};

enum class VolumeSliceAxis : u8
{
    X = 0U,
    Y = 1U,
    Z = 2U
};

struct SurfaceVolumeSolverSettings
{
    bool live{false};
    bool paused{false};
    bool singleStepRequested{false};
    bool resetRequested{false};
    bool followCamera{false};

    f32 timeStepSeconds{1.0F / 60.0F};
    f32 scalarDissipationPerSecond{0.08F};
    f32 velocityDissipationPerSecond{0.04F};
    f32 sourceScale{1.0F};
    u32 iterationsPerFrame{1U};
    f32 gpuBudgetMilliseconds{2.0F};

    // M36 representation policy. RepresentationMode on the authored Volume
    // remains the force/debug authority; these values drive Auto.
    f64 lodLiveDistanceMeters{120.0};
    f64 lodPassiveDistanceMeters{1200.0};
    f32 lodLiveProjectedPixels{96.0F};
    f32 lodPassiveProjectedPixels{12.0F};
    f32 lodHysteresisFraction{0.12F};
    u32 coarseRaymarchSteps{24U};
    u32 passiveRaymarchSteps{8U};

    SurfaceVolumeDebugView debugView{
        SurfaceVolumeDebugView::Off};
    u32 debugLayer{0U};
    VolumeSliceAxis sliceAxis{VolumeSliceAxis::Y};
    world_model::VolumeField debugField{
        world_model::VolumeField::Density};
};

struct SurfaceVolumeSolverDiagnostics
{
    bool eligible{false};
    bool live{false};
    bool paused{false};
    bool steppedThisFrame{false};
    bool resetThisFrame{false};

    u32 iterationsThisFrame{0U};
    u32 requestedIterations{0U};
    u32 scalarChannelsSolved{0U};
    u32 sourceCount{0U};
    u32 effectorCount{0U};
    u32 neighborRecordCount{0U};

    u64 scratchBytes{0U};
    u64 metadataBytes{0U};

    f32 simulatedSeconds{0.0F};
    f32 gpuMilliseconds{0.0F};
    f32 gpuBudgetMilliseconds{0.0F};
    bool gpuTimingValid{false};
};

struct SurfaceVolumeCell
{
    f32 scalar{0.0F};
    math::Float3 velocity{};
};

struct LocalVolumeReferenceCell
{
    f32 density{0.0F};
    f32 temperature{0.0F};
    math::Float3 velocity{};
};

struct SurfaceVolumeReferenceConfig
{
    u32 width{0U};
    u32 height{0U};
    u32 layers{1U};
    f32 cellSizeX{1.0F};
    f32 cellSizeY{1.0F};
    f32 cellSizeZ{1.0F};
    f32 deltaSeconds{1.0F / 60.0F};
    f32 scalarDissipationPerSecond{0.0F};
    f32 velocityDissipationPerSecond{0.0F};
};

// Deterministic CPU reference used to validate the same first-order upwind
// transport model the GPU solver uses.
void StepSurfaceVolumeReference(
    std::span<const SurfaceVolumeCell> input,
    std::span<SurfaceVolumeCell> output,
    const SurfaceVolumeReferenceConfig& config,
    math::Float3 uniformWind = {});

void StepLocalVolumeReference(
    std::span<const LocalVolumeReferenceCell> input,
    std::span<LocalVolumeReferenceCell> output,
    const SurfaceVolumeReferenceConfig& config,
    math::Float3 uniformWind = {});

class SurfaceVolumeSolverService
{
public:
    SurfaceVolumeSolverService(
        rhi::Device& device,
        const shader::Compiler& compiler,
        u32 framesInFlight = 1U);
    ~SurfaceVolumeSolverService();

    SurfaceVolumeSolverService(
        const SurfaceVolumeSolverService&) = delete;
    SurfaceVolumeSolverService& operator=(
        const SurfaceVolumeSolverService&) = delete;

    [[nodiscard]] SurfaceVolumeSolverSettings&
    Settings(
        scene::ObjectId volume);

    [[nodiscard]] SurfaceVolumeSolverDiagnostics
    Diagnostics(
        scene::ObjectId volume) const noexcept;

    void BeginGpuTimingFrame(
        rhi::CommandList& commands,
        u32 frameSlot);

    void ResolveGpuTimingFrame(
        u32 frameSlot);

    void RemoveMissing(
        const scene::ObjectStore& objects);

    void AddPasses(
        render_graph::RenderGraph& graph,
        std::string_view prefix,
        const scene::ObjectStore& objects,
        const world_model::ResolvedVolumeDomain& domain,
        volume_fields::VolumeFieldStorage& storage,
        const volume_fields::ImportedVolumeFields& fields,
        u32 frameSlot = 0U);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string_view
SurfaceVolumeDebugViewName(
    SurfaceVolumeDebugView view) noexcept;

[[nodiscard]] std::string_view
VolumeSliceAxisName(
    VolumeSliceAxis axis) noexcept;
} // namespace orbit::volume_solver

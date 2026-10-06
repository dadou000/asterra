#include <orbit/celestial_clouds/CloudField.hpp>
#include <orbit/celestial_clouds/CloudRenderer.hpp>
#include <orbit/rhi/vulkan/VulkanBackend.hpp>
#include <orbit/shader/dxc/DxcShaderCompiler.hpp>
#include <orbit/terrain/TerrainSource.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>

// CloudRenderer::UpdateLightVolume skips its refresh dispatch once every input has been
// unchanged for a full refresh cycle, because the voxels are a pure function of those inputs
// and would be recomputed to the same bits. This test proves that claim on the GPU: a volume
// forced to the original refresh-every-frame schedule and a volume that uses the skip run in
// lock-step on identical inputs, and every byte of both must match after each kind of change.

namespace
{
using namespace orbit;
using namespace orbit::celestial_clouds;

class ClimateSource final : public terrain::TerrainSource
{
public:
    terrain::TerrainSample Sample(
        const terrain::TerrainQuery& query) const noexcept override
    {
        const auto d = math::Normalize(query.unitDirection);
        return {
            .climate = {
                .temperatureC = 15.0F,
                .humidity = static_cast<f32>(std::clamp(0.55 + 0.35 * d.y, 0.0, 1.0)),
                .precipitation = static_cast<f32>(std::clamp(0.45 + 0.30 * d.x, 0.0, 1.0)),
                .continentality = 0.5F
            }
        };
    }

    terrain::TerrainGenerationRevisions GenerationRevisions() const noexcept override
    {
        return {.climate = 17U};
    }
};

constexpr f64 kRadius = 6.371e6;
constexpr u64 kVolumeBytes = 3ULL * 32ULL * 96ULL * 96ULL * 16ULL;

struct Harness
{
    rhi::Device& device;
    rhi::Queue& queue;
    std::unique_ptr<rhi::CommandAllocator> allocator;
    std::unique_ptr<rhi::CommandList> commands;
    std::unique_ptr<rhi::Fence> fence;
    u64 fenceValue{0};

    Harness(rhi::Device& d, rhi::Queue& q)
        : device(d),
          queue(q),
          allocator(d.CreateCommandAllocator(rhi::QueueType::Graphics)),
          commands(d.CreateCommandList(*allocator)),
          fence(d.CreateFence(0))
    {
    }

    template <typename Recorder>
    void Run(Recorder&& record)
    {
        allocator->Reset();
        commands->Reset(*allocator);
        record(*commands);
        commands->Close();
        queue.Submit(*commands);
        ++fenceValue;
        queue.Signal(*fence, fenceValue);
        fence->Wait(fenceValue);
    }
};

[[nodiscard]] std::unique_ptr<rhi::Buffer> ReadBack(Harness& h, rhi::Buffer& volume)
{
    auto readback = h.device.CreateBuffer({
        .sizeBytes = kVolumeBytes,
        .usage = rhi::BufferUsage::Generic,
        .memory = rhi::MemoryUsage::HostReadback,
        .initialState = rhi::ResourceState::CopyDestination
    });
    h.Run([&](rhi::CommandList& commands)
    {
        commands.Transition(volume, rhi::ResourceState::ShaderResource, rhi::ResourceState::CopySource);
        commands.CopyBuffer(volume, 0, *readback, 0, kVolumeBytes);
        commands.Transition(volume, rhi::ResourceState::CopySource, rhi::ResourceState::ShaderResource);
    });
    return readback;
}

[[nodiscard]] bool VolumesIdentical(Harness& h, rhi::Buffer& a, rhi::Buffer& b)
{
    auto ra = ReadBack(h, a);
    auto rb = ReadBack(h, b);
    const std::byte* pa = ra->Map();
    const std::byte* pb = rb->Map();
    const bool same = std::memcmp(pa, pb, kVolumeBytes) == 0;
    ra->Unmap();
    rb->Unmap();
    return same;
}

struct Scene
{
    CloudLayerParameters layer;
    celestial_atmosphere::AtmosphereParameters atmosphere;
    celestial_atmosphere::AtmosphereRenderView view;
    CloudLab lab;
};

// Advances both volumes one frame with the same inputs; returns whether the skipping volume
// skipped its dispatch.
bool Step(
    Harness& h,
    CloudRenderer& renderer,
    CloudRenderer::LightVolume& reference,
    CloudRenderer::LightVolume& skipping,
    GpuCloudFieldProduct& field,
    const Scene& scene,
    const u32 frame)
{
    h.Run([&](rhi::CommandList& commands)
    {
        renderer.UpdateLightVolume(
            commands, reference, field, kRadius, scene.layer, scene.atmosphere, scene.view,
            scene.lab, frame, true);
        renderer.UpdateLightVolume(
            commands, skipping, field, kRadius, scene.layer, scene.atmosphere, scene.view,
            scene.lab, frame, true);
    });
    return skipping.refreshSkipped;
}

struct Phase
{
    u32 skippedFrames{0};
    bool referenceEverSkipped{false};
};

Phase RunFrames(
    Harness& h,
    CloudRenderer& renderer,
    CloudRenderer::LightVolume& reference,
    CloudRenderer::LightVolume& skipping,
    GpuCloudFieldProduct& field,
    const Scene& scene,
    u32& frame,
    const u32 count)
{
    Phase phase;
    for (u32 i = 0; i < count; ++i)
    {
        if (Step(h, renderer, reference, skipping, field, scene, frame++))
        {
            ++phase.skippedFrames;
        }
        phase.referenceEverSkipped = phase.referenceEverSkipped || reference.refreshSkipped;
    }
    return phase;
}

[[nodiscard]] bool Check(const bool condition, const char* what)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << what << "\n";
    }
    return condition;
}
} // namespace

int main()
{
    const auto device = rhi::vulkan::CreateDevice({.enableValidation = true});
    const shader::dxc::DxcShaderCompiler compiler;
    const auto queue = device->CreateQueue(rhi::QueueType::Graphics);
    Harness h(*device, *queue);

    CloudRenderer renderer(*device, compiler);

    ClimateSource climate;
    Scene scene;
    scene.layer = {
        .semanticIdHigh = 1U,
        .semanticIdLow = 2U,
        .sourceModel = CloudSourceModel::ClimateProcedural,
        .baseAltitudeMeters = 1500.0,
        .topAltitudeMeters = 6500.0,
        .peakOpticalDepth = 6.0,
        .seed = 42U
    };
    const CloudFieldConfig config{.faceResolution = 33U, .footprintScale = 2.0, .timeQuantumMicroseconds = 1'000'000};
    auto productA = BuildCloudField(&climate, nullptr, kRadius, {scene.layer}, {}, config);
    auto productB = BuildCloudField(
        &climate, nullptr, kRadius, {scene.layer}, {.microsecondsFromEpoch = 5'000'000}, config);
    GpuCloudFieldProduct fieldA(*device, productA);
    GpuCloudFieldProduct fieldB(*device, productB);
    if (!Check(fieldA.Fingerprint() != fieldB.Fingerprint(), "the two cloud fields must differ"))
    {
        return 1;
    }

    scene.view.cameraPositionMeters = {kRadius + 120.0, 30.0, 40.0};
    const f32 inv = 1.0F / std::sqrt(0.3F * 0.3F + 0.8F * 0.8F + 0.5F * 0.5F);
    scene.view.sunDirection = {0.3F * inv, 0.8F * inv, 0.5F * inv};

    auto reference = renderer.CreateLightVolume();
    auto skipping = renderer.CreateLightVolume();
    reference->allowRefreshSkip = false;
    skipping->allowRefreshSkip = true;

    u32 frame = 0;

    // 1. Static inputs: the skipping volume must settle, then skip, and still match the reference.
    {
        const Phase phase = RunFrames(h, renderer, *reference, *skipping, fieldA, scene, frame, 70U);
        if (!Check(phase.skippedFrames > 30U, "settled volume must skip most late frames") ||
            !Check(!phase.referenceEverSkipped, "the reference volume must never skip") ||
            !Check(VolumesIdentical(h, *reference->buffer, *skipping->buffer),
                   "static scene: skipped refresh must equal the always-refresh volume"))
        {
            return 2;
        }
    }

    // 2. The sun moves: refreshing must resume at once and the volumes must still match.
    {
        scene.view.sunDirection = {0.7F * inv, 0.4F * inv, 0.6F * inv};
        const bool skippedAfterChange =
            Step(h, renderer, *reference, *skipping, fieldA, scene, frame++);
        if (!Check(!skippedAfterChange, "a changed sun must restart refreshing immediately"))
        {
            return 3;
        }
        const Phase phase = RunFrames(h, renderer, *reference, *skipping, fieldA, scene, frame, 70U);
        if (!Check(phase.skippedFrames > 30U, "volume must settle again after the sun moved") ||
            !Check(VolumesIdentical(h, *reference->buffer, *skipping->buffer),
                   "sun moved: volumes must match"))
        {
            return 4;
        }
    }

    // 3. The camera moves far enough to shift the toroidal window: new edge voxels are filled.
    {
        scene.view.cameraPositionMeters = {kRadius + 120.0, 30.0 + 9000.0, 40.0 - 5000.0};
        const bool skippedAfterMove =
            Step(h, renderer, *reference, *skipping, fieldA, scene, frame++);
        if (!Check(!skippedAfterMove, "a moved camera must restart refreshing immediately"))
        {
            return 5;
        }
        const Phase phase = RunFrames(h, renderer, *reference, *skipping, fieldA, scene, frame, 70U);
        if (!Check(phase.skippedFrames > 30U, "volume must settle again after the camera moved") ||
            !Check(VolumesIdentical(h, *reference->buffer, *skipping->buffer),
                   "camera moved: volumes must match"))
        {
            return 6;
        }
    }

    // 4. The cloud field changes (a weather update builds a new product).
    {
        const bool skippedAfterField =
            Step(h, renderer, *reference, *skipping, fieldB, scene, frame++);
        if (!Check(!skippedAfterField, "a changed cloud field must restart refreshing immediately"))
        {
            return 7;
        }
        const Phase phase = RunFrames(h, renderer, *reference, *skipping, fieldB, scene, frame, 70U);
        if (!Check(phase.skippedFrames > 30U, "volume must settle again after the field changed") ||
            !Check(VolumesIdentical(h, *reference->buffer, *skipping->buffer),
                   "field changed: volumes must match"))
        {
            return 8;
        }
    }

    // 5. A frame index that crosses the 4096 wrap must not leave stale voxels behind: start the
    // window just before the wrap with changed inputs, then compare.
    {
        frame = 4096U - 7U;
        scene.view.sunDirection = {0.2F * inv, 0.9F * inv, 0.3F * inv};
        const Phase phase = RunFrames(h, renderer, *reference, *skipping, fieldB, scene, frame, 80U);
        if (!Check(phase.skippedFrames > 20U, "volume must settle across the frame-index wrap") ||
            !Check(VolumesIdentical(h, *reference->buffer, *skipping->buffer),
                   "inputs changed just before the 4096 frame wrap: volumes must match"))
        {
            return 9;
        }
    }

    std::cout << "CloudLightVolumeTests passed\n";
    return 0;
}

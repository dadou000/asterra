#include <orbit/rhi/vulkan/VulkanBackend.hpp>
#include <orbit/shader/dxc/DxcShaderCompiler.hpp>
#include <orbit/weather_gpu/GpuStormSolver.hpp>
#include <orbit/weather_gpu/NestedGpuSolver.hpp>
#include <orbit/weather_lab/NestedStormSolver.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <iostream>
#include <source_location>

using namespace orbit;
using namespace orbit::weather_lab;

namespace
{
void Check(const bool condition, const std::source_location location = std::source_location::current())
{
    if (!condition)
    {
        std::cerr << "Weather GPU test failed at " << location.file_name() << ':' << location.line() << '\n';
        std::exit(1);
    }
}

double MaxDifference(const float* a, const float* b, const std::size_t n)
{
    double worst = 0.0;
    for (std::size_t i = 0; i < n; ++i)
    {
        worst = std::max(worst, static_cast<double>(std::fabs(a[i] - b[i])));
    }
    return worst;
}

std::vector<float> ReadBuffer(rhi::Device& device, rhi::Buffer& source, const std::size_t floats)
{
    auto queue = device.CreateQueue(rhi::QueueType::Graphics);
    auto fence = device.CreateFence(0);
    auto allocator = device.CreateCommandAllocator(rhi::QueueType::Graphics);
    auto commands = device.CreateCommandList(*allocator);
    auto readback = device.CreateBuffer({.sizeBytes = floats * sizeof(float), .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::HostReadback, .initialState = rhi::ResourceState::CopyDestination});
    allocator->Reset();
    commands->Reset(*allocator);
    commands->Transition(source, rhi::ResourceState::ShaderResource, rhi::ResourceState::CopySource);
    commands->CopyBuffer(source, 0U, *readback, 0U, floats * sizeof(float));
    commands->Transition(source, rhi::ResourceState::CopySource, rhi::ResourceState::ShaderResource);
    commands->Close();
    queue->Submit(*commands);
    queue->Signal(*fence, 1U);
    fence->Wait(1U);
    std::vector<float> out(floats);
    std::memcpy(out.data(), readback->Map(), floats * sizeof(float));
    readback->Unmap();
    return out;
}

FastStormConfig SmallConfig()
{
    FastStormConfig c;
    c.nx = 32;
    c.ny = 32;
    c.nz = 40;
    c.threads = 2;
    c.ice = true;
    c.lateralSpongeCells = 4;
    c.maxTimeStep = 12.0F;
    c.maxCourant = 1.0e9F;   // fixed step, the same on both sides
    return c;
}
} // namespace

int RunTests()
{
    std::unique_ptr<rhi::Device> device;
    try
    {
        device = rhi::vulkan::CreateDevice({});
    }
    catch (const std::exception& exception)
    {
        std::cout << "Weather GPU tests skipped: no Vulkan device (" << exception.what() << ")\n";
        return 0;
    }
    shader::dxc::DxcShaderCompiler compiler;
    const FastStormConfig config = SmallConfig();
    Check(weather_gpu::GpuStormSolver::Validate(config).empty());
    FastStormConfig tooBig = config;
    tooBig.nx = 512;
    Check(!weather_gpu::GpuStormSolver::Validate(tooBig).empty());

    FastStormSolver cpu(config);
    weather_gpu::GpuStormSolver gpu(*device, compiler, config);
    gpu.Upload(cpu);

    // Round trip: what goes up comes back unchanged.
    {
        FastStormSolver back(config);
        gpu.Download(back);
        const std::size_t cells = 32U * 32U * 40U;
        Check(MaxDifference(cpu.MutableState().theta, back.MutableState().theta, cells) == 0.0);
        Check(MaxDifference(cpu.MutableState().u, back.MutableState().u, cells) == 0.0);
    }

    // Eight steps of the storm: the GPU and the CPU reference agree to float32 accuracy of the stages.
    constexpr int kSteps = 8;
    for (int i = 0; i < kSteps; ++i)
    {
        (void)cpu.Step(12.0F);
    }
    gpu.Advance(kSteps, 12.0F);
    Check(std::fabs(gpu.Time() - cpu.Diagnostics().time) < 1.0e-3);
    FastStormSolver downloaded(config);
    gpu.Download(downloaded);
    const FastStormState a = cpu.MutableState();
    const FastStormState b = downloaded.MutableState();
    const std::size_t cells = 32U * 32U * 40U;
    Check(MaxDifference(a.theta, b.theta, cells) < 0.05);
    Check(MaxDifference(a.u, b.u, cells) < 0.05);
    Check(MaxDifference(a.v, b.v, cells) < 0.05);
    Check(MaxDifference(a.w, b.w, cells + 32U * 32U) < 0.2);
    Check(MaxDifference(a.qv, b.qv, cells) < 2.0e-4);
    float wmax = 0.0F;
    for (std::size_t i = 0; i < cells; ++i) { wmax = std::max(wmax, b.w[i]); }
    Check(wmax > 0.1F);   // the bubble is rising (about 0.3 m/s after 96 s)

    // A failed reload keeps the running pipelines; a good one swaps in.
    const std::string error = gpu.ReloadShaders("this is not hlsl");
    Check(!error.empty());
    gpu.Advance(1, 12.0F);
    Check(gpu.ReloadShaders().empty());
    gpu.Advance(1, 12.0F);

    // Cloud export for the renderer: a placement header and a physically sane extinction field.
    gpu.Advance(100, 12.0F);
    weather_gpu::StormPlacement placement;
    placement.centre = {6371000.0, 0.0, 0.0};
    placement.east = {0.0F, 0.0F, 1.0F};
    placement.north = {0.0F, 1.0F, 0.0F};
    gpu.ExportCloud(placement);
    const std::vector<float> out = ReadBuffer(*device, gpu.CloudBuffer(), 32U + cells);
    Check(out[0] == 1.0F);
    Check(out[13] == 6371000.0F && out[14] == config.dx && out[17] == 32.0F && out[19] == 40.0F);
    Check(out[20] > 0.0F);
    float peak = 0.0F;
    double total = 0.0;
    for (std::size_t i = 0; i < cells; ++i)
    {
        Check(std::isfinite(out[32 + i]) && out[32 + i] >= 0.0F);
        peak = std::max(peak, out[32 + i]);
        total += out[32 + i];
    }
    Check(peak > 0.005F && peak < 5.0F);   // a cloud exists, and its extinction is a plausible fraction of 1 per metre
    Check(total > 0.0);

    // Determinism and checkpoints: two independent solvers agree bit for bit, and restoring a mid-run checkpoint and stepping on gives
    // exactly the state of the run that never stopped. This is what lets a storm be recomputed after a timewarp.
    {
        FastStormSolver init(config);
        weather_gpu::GpuStormSolver first(*device, compiler, config);
        weather_gpu::GpuStormSolver second(*device, compiler, config);
        first.Upload(init);
        second.Upload(init);
        first.Advance(40, 12.0F);
        second.Advance(40, 12.0F);
        const weather_gpu::StormCheckpoint ca = first.Save();
        const weather_gpu::StormCheckpoint cb = second.Save();
        Check(ca.data == cb.data && ca.time == cb.time);                 // two runs, one answer
        first.Advance(25, 12.0F);
        const weather_gpu::StormCheckpoint straight = first.Save();
        weather_gpu::GpuStormSolver c(*device, compiler, config);
        c.Restore(ca);                                                   // into a solver that never ran
        Check(c.Time() == ca.time);
        c.Advance(25, 12.0F);
        Check(c.Save().data == straight.data);                           // restore then step == never stopped
        // Restoring into the original solver after it moved on also works.
        first.Restore(ca);
        first.Advance(25, 12.0F);
        Check(first.Save().data == straight.data);
        Check(c.CheckpointBytes() > 1000000U);
        weather_gpu::StormCheckpoint wrong;
        wrong.data.assign(10, 0.0F);
        bool threw = false;
        try { c.Restore(wrong); } catch (const std::exception&) { threw = true; }
        Check(threw);
    }

    // The nest on the GPU against the nest on the CPU, with the same step schedule on both (parent 12 s, one child step of 12 s).
    {
        weather_lab::NestConfig cpuNest;
        cpuNest.parent = config;
        cpuNest.parent.lateralSpongeCells = 0;
        cpuNest.parent.maxTimeStep = 12.0F;
        cpuNest.ratio = 2;
        cpuNest.cells = 16;
        cpuNest.relaxCells = 6;
        weather_lab::NestedStormSolver cpuSolver(cpuNest);
        weather_gpu::NestedGpuConfig gpuNest;
        gpuNest.parent = cpuNest.parent;
        gpuNest.ratio = 2;
        gpuNest.cells = 16;
        gpuNest.relaxCells = 6;
        gpuNest.childSubsteps = 1;
        Check(weather_gpu::NestedGpuSolver::Validate(gpuNest).empty());
        weather_gpu::NestedGpuSolver gpuSolver(*device, compiler, gpuNest);
        gpuSolver.Upload(cpuSolver.Parent());
        cpuSolver.Advance(480.0);          // 40 parent steps
        gpuSolver.Advance(40, 12.0F);
        Check(std::fabs(gpuSolver.Time() - cpuSolver.Parent().Diagnostics().time) < 1.0e-3);
        FastStormSolver parentBack(cpuNest.parent);
        gpuSolver.Parent().Download(parentBack);
        const FastStormState pa = cpuSolver.Parent().MutableState();
        const FastStormState pb = parentBack.MutableState();
        const std::size_t parentCells = 32U * 32U * 40U;
        std::cout << "nest parent diff: theta " << MaxDifference(pa.theta, pb.theta, parentCells) << " u " << MaxDifference(pa.u, pb.u, parentCells)
                  << " w " << MaxDifference(pa.w, pb.w, parentCells) << '\n';
        Check(MaxDifference(pa.theta, pb.theta, parentCells) < 0.05);
        FastStormConfig childConfig = cpuNest.parent;
        childConfig.nx = 32;
        childConfig.ny = 32;
        childConfig.dx = cpuNest.parent.dx / 2.0F;
        childConfig.dy = childConfig.dx;
        FastStormSolver childBack(childConfig);
        gpuSolver.Child().Download(childBack);
        const FastStormState ca = cpuSolver.Child().MutableState();
        const FastStormState cb = childBack.MutableState();
        const std::size_t childCells = 32U * 32U * 40U;
        std::cout << "nest child diff:  theta " << MaxDifference(ca.theta, cb.theta, childCells) << " u " << MaxDifference(ca.u, cb.u, childCells)
                  << " w " << MaxDifference(ca.w, cb.w, childCells) << " qv " << MaxDifference(ca.qv, cb.qv, childCells) << '\n';
        Check(MaxDifference(ca.theta, cb.theta, childCells) < 0.1);
        Check(MaxDifference(ca.u, cb.u, childCells) < 0.1);
        Check(MaxDifference(ca.w, cb.w, childCells + 32U * 32U) < 0.2);
        Check(MaxDifference(ca.qv, cb.qv, childCells) < 5.0e-4);
        // The child has its own storm development (it is finer than the parent), not a copy of the parent's field.
        float childW = 0.0F;
        for (std::size_t i = 0; i < childCells; ++i) { childW = std::max(childW, cb.w[i]); }
        Check(childW > 0.05F);

        // A nest checkpoint (both levels) restores and continues bit for bit; a nest is deterministic.
        gpuSolver.Advance(10, 12.0F);
        const weather_gpu::StormCheckpoint saved = gpuSolver.Save();
        Check(saved.data.size() * sizeof(float) == gpuSolver.CheckpointBytes());
        gpuSolver.Advance(10, 12.0F);
        const weather_gpu::StormCheckpoint straight = gpuSolver.Save();
        weather_gpu::NestedGpuSolver replay(*device, compiler, gpuNest);
        replay.Restore(saved);
        replay.Advance(10, 12.0F);
        Check(replay.Save().data == straight.data);
    }

    std::cout << "Weather GPU tests passed\n";
    return 0;
}

int main()
{
    try
    {
        return RunTests();
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Weather GPU test threw: " << exception.what() << std::endl;
        return 1;
    }
}

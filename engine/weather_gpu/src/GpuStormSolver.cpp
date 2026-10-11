#include <orbit/weather_gpu/GpuStormSolver.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace orbit::weather_gpu
{
using weather_lab::FastStormConfig;
using weather_lab::FastStormSolver;

namespace
{
constexpr double kPi = 3.14159265358979323846;

// Mirrors `struct Params` in storm.hlsl: 36 32-bit scalars, in the same order.
struct Params
{
    u32 nx, ny, nz, plane;
    u32 cells, faceCells, flags, nScalars;
    f32 dx, dy, dz, dt;
    f32 frameU, frameV, ch, cv;
    u32 offU, offV, offW, offS;
    u32 dstU, dstV, dstW, dstS;
    u32 offRain, offSpec, offPhi, offAux;
    u32 arg0, arg1, arg2, arg3;
    f32 spongeBase, spongeRate, latCells, latRate;
};
static_assert(sizeof(Params) == 36U * 4U);

// The entry points of storm.hlsl.
constexpr std::array<const char*, 15> kKernels{
    "AdvectVelocity", "AdvectScalars", "MassPartial", "MassRatio", "MassScale", "Microphysics", "Buoyancy", "TopSponge", "Mix",
    "LateralSponge", "ProjRhs", "FftLine", "ProjTridiag", "ProjGradient", "ExportCloud"};

std::string ReadFile(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return {};
    }
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}
} // namespace

struct GpuStormSolver::Impl
{
    rhi::Device& device;
    const shader::Compiler& compiler;
    FastStormConfig cfg;

    u32 plane = 0;
    u32 cells = 0;
    u32 faceCells = 0;
    u32 nScalars = 1;
    u64 copyFloats = 0;
    std::array<u32, 2> copyBase{};   // offset of each ping-pong copy
    u32 offRain = 0;
    u32 offSpec = 0;
    u32 offPhi = 0;
    u32 offAux = 0;
    u64 totalFloats = 0;
    u32 current = 0;
    double time = 0.0;
    double lastMs = 0.0;
    std::size_t bytes = 0;

    std::unique_ptr<rhi::Buffer> arena;
    std::unique_ptr<rhi::Buffer> baseBuffer;
    std::unique_ptr<rhi::Buffer> staging;
    std::unique_ptr<rhi::Buffer> readback;
    std::unique_ptr<rhi::Buffer> cloudOut;      // 32 header floats + extinction per cell, for the cloud renderer
    std::unique_ptr<rhi::Buffer> headerStage;
    std::unique_ptr<rhi::Queue> queue;
    std::unique_ptr<rhi::Fence> fence;
    std::unique_ptr<rhi::CommandAllocator> allocator;
    std::unique_ptr<rhi::CommandList> commands;
    u64 fenceValue = 0;
    std::map<std::string, std::unique_ptr<rhi::ComputePipeline>> pipelines;

    Impl(rhi::Device& d, const shader::Compiler& c, const FastStormConfig& config) : device(d), compiler(c), cfg(config)
    {
        plane = cfg.nx * cfg.ny;
        cells = plane * cfg.nz;
        faceCells = plane * (cfg.nz + 1U);
        nScalars = cfg.moisture ? (cfg.ice ? 7U : 4U) : 1U;
        copyFloats = 2ULL * cells + faceCells + static_cast<u64>(nScalars) * cells;
        copyBase = {0U, static_cast<u32>(copyFloats)};
        offRain = static_cast<u32>(2ULL * copyFloats);
        offSpec = offRain + plane;
        offPhi = offSpec + 2U * cells;
        offAux = offPhi + cells;
        const u64 auxFloats = 2ULL * (nScalars > 0U ? nScalars - 1U : 0U) * cfg.nz + nScalars + 8U;
        totalFloats = static_cast<u64>(offAux) + auxFloats;
        bytes = static_cast<std::size_t>(totalFloats * sizeof(f32));

        arena = device.CreateBuffer({.sizeBytes = totalFloats * sizeof(f32), .usage = rhi::BufferUsage::Structured,
            .memory = rhi::MemoryUsage::GpuOnly, .initialState = rhi::ResourceState::UnorderedAccess});
        staging = device.CreateBuffer({.sizeBytes = copyFloats * sizeof(f32), .usage = rhi::BufferUsage::Structured,
            .memory = rhi::MemoryUsage::HostVisible, .initialState = rhi::ResourceState::CopySource});
        readback = device.CreateBuffer({.sizeBytes = copyFloats * sizeof(f32), .usage = rhi::BufferUsage::Structured,
            .memory = rhi::MemoryUsage::HostReadback, .initialState = rhi::ResourceState::CopyDestination});
        cloudOut = device.CreateBuffer({.sizeBytes = (32ULL + cells) * sizeof(f32), .usage = rhi::BufferUsage::Structured,
            .memory = rhi::MemoryUsage::GpuOnly, .initialState = rhi::ResourceState::ShaderResource});
        headerStage = device.CreateBuffer({.sizeBytes = 32ULL * sizeof(f32), .usage = rhi::BufferUsage::Structured,
            .memory = rhi::MemoryUsage::HostVisible, .initialState = rhi::ResourceState::CopySource});
        bytes += static_cast<std::size_t>((32ULL + cells) * sizeof(f32));
        if (!arena || !staging || !readback || !cloudOut || !headerStage)
        {
            throw std::runtime_error("GpuStormSolver: could not allocate the state buffers");
        }
        BuildBaseBuffer();
        queue = device.CreateQueue(rhi::QueueType::Graphics);
        fence = device.CreateFence(0);
        allocator = device.CreateCommandAllocator(rhi::QueueType::Graphics);
        commands = device.CreateCommandList(*allocator);
        if (const std::string error = Compile({}, pipelines); !error.empty())
        {
            throw std::runtime_error("GpuStormSolver: shader compile failed: " + error);
        }
    }

    void BuildBaseBuffer()
    {
        weather_lab::BaseState base = weather_lab::BuildSupercellBaseState(cfg.sounding, cfg.nz, cfg.dz);
        if (!cfg.moisture)
        {
            std::fill(base.vapor.begin(), base.vapor.end(), 0.0F);
        }
        const u32 nz = cfg.nz;
        std::vector<f32> data;
        data.reserve(11U * nz + 2U + cfg.nx + cfg.ny);
        for (const auto* table : {&base.theta, &base.vapor, &base.exner, &base.pressure, &base.density, &base.windU, &base.windV})
        {
            data.insert(data.end(), table->begin(), table->begin() + nz);
        }
        data.insert(data.end(), base.faceDensity.begin(), base.faceDensity.begin() + nz + 1U);
        const double dz2 = static_cast<double>(cfg.dz) * cfg.dz;
        std::vector<f32> lower(nz), upper(nz);
        for (u32 k = 0; k < nz; ++k)
        {
            const double rc = base.density[k];
            lower[k] = k == 0U ? 0.0F : static_cast<f32>(base.faceDensity[k] / (rc * dz2));
            upper[k] = k + 1U == nz ? 0.0F : static_cast<f32>(base.faceDensity[k + 1U] / (rc * dz2));
        }
        data.insert(data.end(), lower.begin(), lower.end());
        data.insert(data.end(), upper.begin(), upper.end());
        for (u32 m = 0; m < cfg.nx; ++m)
        {
            data.push_back(static_cast<f32>((2.0 - 2.0 * std::cos(2.0 * kPi * m / cfg.nx)) / (static_cast<double>(cfg.dx) * cfg.dx)));
        }
        for (u32 m = 0; m < cfg.ny; ++m)
        {
            data.push_back(static_cast<f32>((2.0 - 2.0 * std::cos(2.0 * kPi * m / cfg.ny)) / (static_cast<double>(cfg.dy) * cfg.dy)));
        }
        baseBuffer = device.CreateBuffer({.sizeBytes = data.size() * sizeof(f32), .usage = rhi::BufferUsage::Structured,
            .memory = rhi::MemoryUsage::HostVisible, .initialState = rhi::ResourceState::ShaderResource});
        if (!baseBuffer)
        {
            throw std::runtime_error("GpuStormSolver: could not allocate the base-state buffer");
        }
        std::memcpy(baseBuffer->Map(), data.data(), data.size() * sizeof(f32));
        baseBuffer->Unmap();
        bytes += data.size() * sizeof(f32);
    }

    // Compiles every kernel into `into`; the caller swaps it in only on success.
    std::string Compile(std::string_view source, std::map<std::string, std::unique_ptr<rhi::ComputePipeline>>& into)
    {
        std::string text(source);
        if (text.empty())
        {
            char* path = nullptr;
            std::size_t length = 0;
            if (_dupenv_s(&path, &length, "ORBIT_WEATHER_GPU_SHADER") == 0 && path != nullptr)
            {
                if (*path != '\0')
                {
                    text = ReadFile(path);
                }
                std::free(path);
            }
            if (text.empty())
            {
                text = std::string(EmbeddedStormShader());
            }
        }
        std::map<std::string, std::unique_ptr<rhi::ComputePipeline>> built;
        const char* failing = "";
        try
        {
            for (const char* name : kKernels)
            {
                failing = name;
                if (*name == '\0')
                {
                    continue;
                }
                // The RHI creates every compute pipeline with the entry point `main`: expose this kernel under that name.
                std::string defines;
                for (const char* other : kKernels)
                {
                    defines += std::string("#define ENTRY_") + other
                        + (std::string(other) == name ? std::string(" main") : " Unused_" + std::string(other)) + '\n';
                }
                const std::string full = defines + text;
                const auto binary = compiler.Compile({.source = full, .entryPoint = "main", .stage = shader::Stage::Compute, .debug = false});
                built[name] = device.CreateComputePipeline({
                    .computeShader = {.data = binary.bytecode.data(), .size = binary.bytecode.size()},
                    .pushConstantDwords = static_cast<u32>(sizeof(Params) / 4U),
                    .shaderResourceBuffers = 3U});
            }
        }
        catch (const std::exception& exception)
        {
            return std::string(failing) + ": " + exception.what();
        }
        into = std::move(built);
        return {};
    }

    [[nodiscard]] Params Base() const
    {
        Params p{};
        p.nx = cfg.nx;
        p.ny = cfg.ny;
        p.nz = cfg.nz;
        p.plane = plane;
        p.cells = cells;
        p.faceCells = faceCells;
        p.flags = (cfg.advection == weather_lab::AdvectionScheme::MonotoneCubic ? 1U : 0U) | (cfg.moisture ? 2U : 0U)
            | (cfg.ice ? 4U : 0U);
        p.nScalars = nScalars;
        p.dx = cfg.dx;
        p.dy = cfg.dy;
        p.dz = cfg.dz;
        p.frameU = cfg.frameU;
        p.frameV = cfg.frameV;
        p.offRain = offRain;
        p.offSpec = offSpec;
        p.offPhi = offPhi;
        p.offAux = offAux;
        p.spongeBase = cfg.spongeBase;
        p.spongeRate = cfg.spongeRate;
        p.latCells = static_cast<f32>(cfg.lateralSpongeCells);
        p.latRate = cfg.lateralSpongeRate;
        SetCopies(p);
        return p;
    }

    void SetCopies(Params& p) const
    {
        const u32 cur = copyBase[current];
        const u32 other = copyBase[1U - current];
        p.offU = cur;
        p.offV = cur + cells;
        p.offW = cur + 2U * cells;
        p.offS = cur + 2U * cells + faceCells;
        p.dstU = other;
        p.dstV = other + cells;
        p.dstW = other + 2U * cells;
        p.dstS = other + 2U * cells + faceCells;
    }

    void Run(const char* kernel, const Params& p, const u32 gx, const u32 gy, const u32 gz)
    {
        commands->SetComputePipeline(*pipelines.at(kernel));
        commands->SetComputeConstants(std::span<const u32>(reinterpret_cast<const u32*>(&p), sizeof(Params) / 4U));
        commands->SetComputeBuffer(0U, *arena);
        commands->SetComputeBuffer(1U, *baseBuffer);
        commands->SetComputeBuffer(2U, *arena);   // the cloud output slot: only ExportCloud uses it, which binds the real buffer
        commands->Dispatch(gx, gy, gz);
        commands->UavBarrier(*arena);
    }

    void RecordStep(const f32 dt)
    {
        const u32 gx = (cfg.nx + 7U) / 8U;
        const u32 gy = (cfg.ny + 7U) / 8U;
        const u32 nz = cfg.nz;
        Params p = Base();
        p.dt = dt;

        // Advection into the other copy.
        for (u32 mode = 0; mode < 3U; ++mode)
        {
            p.arg0 = mode;
            Run("AdvectVelocity", p, gx, gy, mode == 2U ? nz + 1U : nz);
        }
        Run("AdvectScalars", p, gx, gy, nz);
        if (cfg.moisture && cfg.massFixer && nScalars > 1U)
        {
            Run("MassPartial", p, nz, 2U * (nScalars - 1U), 1U);
            Run("MassRatio", p, 1U, 1U, 1U);
            Run("MassScale", p, gx, gy, nz);
        }
        current = 1U - current;
        p = Base();
        p.dt = dt;

        if (cfg.moisture)
        {
            Run("Microphysics", p, (plane + 63U) / 64U, 1U, 1U);
        }
        Run("Buoyancy", p, gx, gy, nz - 1U);
        Run("TopSponge", p, gx, gy, nz + 1U);

        p.ch = cfg.horizontalMixing * dt / (cfg.dx * cfg.dx);
        p.cv = cfg.verticalMixing * dt / (cfg.dz * cfg.dz);
        if (p.ch > 0.0F || p.cv > 0.0F)
        {
            struct Job { u32 srcOffset; u32 dstOffset; u32 kind; u32 isW; u32 levels; };
            const u32 other = copyBase[1U - current];
            const u32 cur = copyBase[current];
            std::vector<Job> jobs{
                {cur, other, 3U, 0U, nz},
                {cur + cells, other + cells, 4U, 0U, nz},
                {cur + 2U * cells, other + 2U * cells, 0U, 1U, nz + 1U},
                {cur + 2U * cells + faceCells, other + 2U * cells + faceCells, 1U, 0U, nz}};
            if (cfg.moisture)
            {
                for (u32 s = 1U; s < nScalars; ++s)
                {
                    jobs.push_back({cur + 2U * cells + faceCells + s * cells, other + 2U * cells + faceCells + s * cells,
                        s == 1U ? 2U : 0U, 0U, nz});
                }
            }
            for (const Job& job : jobs)
            {
                p.arg0 = job.srcOffset;
                p.arg1 = job.dstOffset;
                p.arg2 = job.kind;
                p.arg3 = job.isW;
                Run("Mix", p, gx, gy, job.levels);
            }
            current = 1U - current;
            p = Base();
            p.dt = dt;
        }

        if (cfg.lateralSpongeCells > 0U)
        {
            Run("LateralSponge", p, gx, gy, nz);
        }

        // Pressure projection.
        Run("ProjRhs", p, gx, gy, nz);
        p.arg0 = 0U; p.arg1 = 0U; p.arg2 = 0U;
        Run("FftLine", p, cfg.ny, nz, 1U);          // forward, along x
        p.arg0 = 1U;
        Run("FftLine", p, cfg.nx, nz, 1U);          // forward, along y
        Run("ProjTridiag", p, (plane + 63U) / 64U, 1U, 1U);
        p.arg0 = 1U; p.arg1 = 1U; p.arg2 = 0U;
        Run("FftLine", p, cfg.nx, nz, 1U);          // inverse, along y
        p.arg0 = 0U; p.arg1 = 1U; p.arg2 = 1U;
        Run("FftLine", p, cfg.ny, nz, 1U);          // inverse, along x, writing phi
        Run("ProjGradient", p, gx, gy, nz);
        time += dt;
    }

    void Submit()
    {
        commands->Close();
        queue->Submit(*commands);
        queue->Signal(*fence, ++fenceValue);
        fence->Wait(fenceValue);
    }

    void Begin()
    {
        allocator->Reset();
        commands->Reset(*allocator);
    }

    // Raw pointers of the CPU solver's arrays, in this solver's copy layout: u, v, w, then scalars.
    struct Layout
    {
        std::array<float*, 3 + 7> pointers;
        std::array<std::size_t, 3 + 7> sizes;
    };

    [[nodiscard]] Layout LayoutOf(FastStormSolver& cpu) const
    {
        const weather_lab::FastStormState s = cpu.MutableState();
        Layout layout{};
        layout.pointers = {s.u, s.v, s.w, s.theta, s.qv, s.qc, s.qr, s.qi, s.qs, s.qg};
        for (std::size_t i = 0; i < layout.sizes.size(); ++i)
        {
            layout.sizes[i] = i == 2U ? faceCells : cells;
        }
        return layout;
    }
};

GpuStormSolver::GpuStormSolver(rhi::Device& device, const shader::Compiler& compiler, const FastStormConfig& config)
    : impl_(std::make_unique<Impl>(device, compiler, config))
{
}

GpuStormSolver::~GpuStormSolver() = default;

std::string GpuStormSolver::Validate(const FastStormConfig& config)
{
    if (const std::string problem = FastStormSolver::Validate(config); !problem.empty())
    {
        return problem;
    }
    if (config.nx > 256U || config.ny > 256U)
    {
        return "the GPU FFT handles at most 256 cells per axis";
    }
    if (config.nz > 120U)
    {
        return "the GPU kernels handle at most 120 levels";
    }
    if (config.trackStorm)
    {
        return "the storm-following frame needs a readback and is CPU only for now";
    }
    return {};
}

std::string GpuStormSolver::ReloadShaders(const std::string_view source)
{
    std::map<std::string, std::unique_ptr<rhi::ComputePipeline>> built;
    if (const std::string error = impl_->Compile(source, built); !error.empty())
    {
        return error;   // the running pipelines stay in use
    }
    // The old pipelines are not in flight: every Advance waits for its fence before returning.
    impl_->pipelines = std::move(built);
    return {};
}

void GpuStormSolver::Upload(FastStormSolver& cpu)
{
    Impl& s = *impl_;
    const Impl::Layout layout = s.LayoutOf(cpu);
    f32* mapped = reinterpret_cast<f32*>(s.staging->Map());
    std::size_t at = 0;
    const std::size_t count = 3U + s.nScalars;
    for (std::size_t i = 0; i < count; ++i)
    {
        std::memcpy(mapped + at, layout.pointers[i], layout.sizes[i] * sizeof(f32));
        at += layout.sizes[i];
    }
    s.staging->Unmap();
    s.current = 0U;
    s.time = cpu.Diagnostics().time;
    s.Begin();
    s.commands->Transition(*s.arena, rhi::ResourceState::UnorderedAccess, rhi::ResourceState::CopyDestination);
    s.commands->CopyBuffer(*s.staging, 0U, *s.arena, 0U, s.copyFloats * sizeof(f32));
    s.commands->Transition(*s.arena, rhi::ResourceState::CopyDestination, rhi::ResourceState::UnorderedAccess);
    s.Submit();
}

void GpuStormSolver::Download(FastStormSolver& cpu)
{
    Impl& s = *impl_;
    s.Begin();
    s.commands->Transition(*s.arena, rhi::ResourceState::UnorderedAccess, rhi::ResourceState::CopySource);
    s.commands->CopyBuffer(*s.arena, static_cast<u64>(s.copyBase[s.current]) * sizeof(f32), *s.readback, 0U,
        s.copyFloats * sizeof(f32));
    s.commands->Transition(*s.arena, rhi::ResourceState::CopySource, rhi::ResourceState::UnorderedAccess);
    s.Submit();
    const Impl::Layout layout = s.LayoutOf(cpu);
    const f32* mapped = reinterpret_cast<const f32*>(s.readback->Map());
    std::size_t at = 0;
    const std::size_t count = 3U + s.nScalars;
    for (std::size_t i = 0; i < count; ++i)
    {
        if (layout.pointers[i] != nullptr)
        {
            std::memcpy(layout.pointers[i], mapped + at, layout.sizes[i] * sizeof(f32));
        }
        at += layout.sizes[i];
    }
    s.readback->Unmap();
}

void GpuStormSolver::Advance(const u32 steps, const f32 dt)
{
    Impl& s = *impl_;
    const auto start = std::chrono::steady_clock::now();
    s.Begin();
    for (u32 i = 0; i < steps; ++i)
    {
        s.RecordStep(dt);
    }
    s.Submit();
    s.lastMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

rhi::Buffer& GpuStormSolver::CloudBuffer() { return *impl_->cloudOut; }

void GpuStormSolver::ExportCloud(const StormPlacement& placement)
{
    Impl& s = *impl_;
    std::array<f32, 32> header{};
    header[0] = placement.active ? 1.0F : 0.0F;
    for (std::size_t i = 0; i < 3; ++i)
    {
        header[1 + i] = static_cast<f32>(placement.centre[i]);
        header[4 + i] = placement.east[i];
        header[7 + i] = placement.north[i];
    }
    header[13] = placement.planetRadius;
    header[14] = s.cfg.dx;
    header[15] = s.cfg.dy;
    header[16] = s.cfg.dz;
    header[17] = static_cast<f32>(s.cfg.nx);
    header[18] = static_cast<f32>(s.cfg.ny);
    header[19] = static_cast<f32>(s.cfg.nz);
    const f32 sponge = static_cast<f32>(s.cfg.lateralSpongeCells > 0U ? s.cfg.lateralSpongeCells : 8U) * s.cfg.dx;
    header[20] = placement.fadeMeters > 0.0F ? placement.fadeMeters : sponge;
    std::memcpy(s.headerStage->Map(), header.data(), header.size() * sizeof(f32));
    s.headerStage->Unmap();

    s.Begin();
    s.commands->Transition(*s.cloudOut, rhi::ResourceState::ShaderResource, rhi::ResourceState::CopyDestination);
    s.commands->CopyBuffer(*s.headerStage, 0U, *s.cloudOut, 0U, header.size() * sizeof(f32));
    s.commands->Transition(*s.cloudOut, rhi::ResourceState::CopyDestination, rhi::ResourceState::UnorderedAccess);
    Params p = s.Base();
    s.commands->SetComputePipeline(*s.pipelines.at("ExportCloud"));
    s.commands->SetComputeConstants(std::span<const u32>(reinterpret_cast<const u32*>(&p), sizeof(Params) / 4U));
    s.commands->SetComputeBuffer(0U, *s.arena);
    s.commands->SetComputeBuffer(1U, *s.baseBuffer);
    s.commands->SetComputeBuffer(2U, *s.cloudOut);
    s.commands->Dispatch((s.cfg.nx + 7U) / 8U, (s.cfg.ny + 7U) / 8U, s.cfg.nz);
    s.commands->UavBarrier(*s.cloudOut);
    s.commands->Transition(*s.cloudOut, rhi::ResourceState::UnorderedAccess, rhi::ResourceState::ShaderResource);
    s.Submit();
}

double GpuStormSolver::Time() const { return impl_->time; }
double GpuStormSolver::LastAdvanceMilliseconds() const { return impl_->lastMs; }
std::size_t GpuStormSolver::ResidentBytes() const { return impl_->bytes; }
const FastStormConfig& GpuStormSolver::Config() const { return impl_->cfg; }
} // namespace orbit::weather_gpu

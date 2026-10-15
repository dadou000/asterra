// orbit_weather_gpu: runs the GPU storm core against the CPU reference and times it.
//
//   orbit_weather_gpu compare [--nx 32 --ny 32 --nz 40 --steps 12 --dt 12 --ice 1 --sponge 0]
//   orbit_weather_gpu bench   [--nx 256 --ny 256 --steps 40 --dt 24 --ice 1 --sponge 16]
//   orbit_weather_gpu nest    [--nx 128 --ny 128 --ratio 4 --cells 32 --steps 40 --dt 24 --ice 1 --sponge 12]

#include <orbit/rhi/vulkan/VulkanBackend.hpp>
#include <orbit/shader/dxc/DxcShaderCompiler.hpp>
#include <orbit/weather_gpu/GpuStormSolver.hpp>
#include <orbit/weather_gpu/NestedGpuSolver.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace orbit;
using namespace orbit::weather_lab;

namespace
{
using Options = std::map<std::string, std::string>;

Options ParseOptions(const int argc, char** argv, const int first)
{
    Options options;
    for (int i = first; i + 1 < argc; i += 2)
    {
        const std::string key = argv[i];
        if (key.rfind("--", 0) == 0)
        {
            options[key.substr(2)] = argv[i + 1];
        }
    }
    return options;
}

double Number(const Options& o, const char* key, const double fallback)
{
    const auto it = o.find(key);
    return it == o.end() ? fallback : std::atof(it->second.c_str());
}

FastStormConfig ConfigFrom(const Options& o, const double defaultNx, const double defaultSponge)
{
    FastStormConfig c;
    c.nx = static_cast<std::uint32_t>(Number(o, "nx", defaultNx));
    c.ny = static_cast<std::uint32_t>(Number(o, "ny", defaultNx));
    c.nz = static_cast<std::uint32_t>(Number(o, "nz", 40.0));
    c.ice = Number(o, "ice", 1.0) != 0.0;
    c.lateralSpongeCells = static_cast<std::uint32_t>(Number(o, "sponge", defaultSponge));
    c.bubble.amplitude = static_cast<float>(Number(o, "bubble", 1.0));
    c.threads = static_cast<std::uint32_t>(Number(o, "threads", 8.0));
    // The GPU takes a fixed step; make the CPU reference take exactly the same one.
    c.maxTimeStep = static_cast<float>(Number(o, "dt", 24.0));
    c.maxCourant = 1.0e9F;
    return c;
}

// Max |divergence| of rho0 u on the periodic grid, from raw state arrays.
double MaxDivergence(FastStormSolver& solver)
{
    const FastStormConfig& c = solver.Config();
    const FastStormState s = solver.MutableState();
    const BaseState& base = solver.Base();
    double worst = 0.0;
    for (std::uint32_t k = 0; k < c.nz; ++k)
    {
        for (std::uint32_t j = 0; j < c.ny; ++j)
        {
            for (std::uint32_t i = 0; i < c.nx; ++i)
            {
                const std::size_t idx = (static_cast<std::size_t>(k) * c.ny + j) * c.nx + i;
                const std::size_t ip = (static_cast<std::size_t>(k) * c.ny + j) * c.nx + ((i + 1U) & (c.nx - 1U));
                const std::size_t jp = (static_cast<std::size_t>(k) * c.ny + ((j + 1U) & (c.ny - 1U))) * c.nx + i;
                const std::size_t up = idx + static_cast<std::size_t>(c.nx) * c.ny;
                const double div = (s.u[ip] - s.u[idx]) / c.dx + (s.v[jp] - s.v[idx]) / c.dy
                    + (base.faceDensity[k + 1U] * s.w[up] - base.faceDensity[k] * s.w[idx]) / base.density[k] / c.dz;
                worst = std::max(worst, std::fabs(div));
            }
        }
    }
    return worst;
}

std::unique_ptr<rhi::Device> OpenDevice()
{
    try
    {
        return rhi::vulkan::CreateDevice({});
    }
    catch (const std::exception& exception)
    {
        std::cerr << "no Vulkan device: " << exception.what() << '\n';
        return nullptr;
    }
}

int Compare(const Options& o)
{
    auto device = OpenDevice();
    if (!device) { return 3; }
    shader::dxc::DxcShaderCompiler compiler;
    const FastStormConfig config = ConfigFrom(o, 32.0, 0.0);
    if (const std::string problem = weather_gpu::GpuStormSolver::Validate(config); !problem.empty())
    {
        std::cerr << problem << '\n';
        return 2;
    }
    const auto steps = static_cast<std::uint32_t>(Number(o, "steps", 12.0));
    const float dt = config.maxTimeStep;

    FastStormSolver cpu(config);
    weather_gpu::GpuStormSolver gpu(*device, compiler, config);
    gpu.Upload(cpu);
    std::printf("adapter: %.*s\n", static_cast<int>(device->AdapterName().size()), device->AdapterName().data());
    for (std::uint32_t chunk = 0; chunk < steps; ++chunk)
    {
        (void)cpu.Step(dt);
        gpu.Advance(1U, dt);
        if ((chunk + 1U) % std::max(1U, steps / 6U) != 0U && chunk + 1U != steps) { continue; }
        FastStormSolver downloaded(config);
        gpu.Download(downloaded);
        const FastStormState a = cpu.MutableState();
        const FastStormState b = downloaded.MutableState();
        const auto diff = [&](const char* name, const float* x, const float* y, const std::size_t n)
        {
            if (x == nullptr || y == nullptr) { return; }
            double worst = 0.0, range = 0.0, mean = 0.0;
            for (std::size_t i = 0; i < n; ++i)
            {
                worst = std::max(worst, static_cast<double>(std::fabs(x[i] - y[i])));
                range = std::max(range, static_cast<double>(std::fabs(x[i])));
                mean += std::fabs(x[i] - y[i]);
            }
            std::printf("  %-6s max diff %.3e (field max %.3e, mean diff %.3e)\n", name, worst, range, mean / static_cast<double>(n));
        };
        const std::size_t cells = static_cast<std::size_t>(config.nx) * config.ny * config.nz;
        std::printf("step %u (t = %.0f s): divergence cpu %.2e  gpu %.2e\n", chunk + 1U, cpu.Diagnostics().time, MaxDivergence(cpu),
            MaxDivergence(downloaded));
        diff("u", a.u, b.u, cells);
        diff("v", a.v, b.v, cells);
        diff("w", a.w, b.w, cells + static_cast<std::size_t>(config.nx) * config.ny);
        diff("theta", a.theta, b.theta, cells);
        diff("qv", a.qv, b.qv, cells);
        diff("qc", a.qc, b.qc, cells);
        diff("qr", a.qr, b.qr, cells);
        if (config.ice)
        {
            diff("qi", a.qi, b.qi, cells);
            diff("qs", a.qs, b.qs, cells);
            diff("qg", a.qg, b.qg, cells);
        }
    }
    return 0;
}

int Bench(const Options& o)
{
    auto device = OpenDevice();
    if (!device) { return 3; }
    shader::dxc::DxcShaderCompiler compiler;
    const FastStormConfig config = ConfigFrom(o, 256.0, 16.0);
    if (const std::string problem = weather_gpu::GpuStormSolver::Validate(config); !problem.empty())
    {
        std::cerr << problem << '\n';
        return 2;
    }
    const auto steps = static_cast<std::uint32_t>(Number(o, "steps", 40.0));
    const float dt = config.maxTimeStep;
    FastStormSolver cpu(config);
    weather_gpu::GpuStormSolver gpu(*device, compiler, config);
    gpu.Upload(cpu);
    gpu.Advance(2U, dt);   // warm up
    double best = 1.0e30;
    for (int run = 0; run < 3; ++run)
    {
        gpu.Advance(steps, dt);
        best = std::min(best, gpu.LastAdvanceMilliseconds());
    }
    const double perStep = best / steps;
    std::printf("adapter: %.*s\n", static_cast<int>(device->AdapterName().size()), device->AdapterName().data());
    std::printf("%ux%ux%u (%.2f M cells), %u steps of %.0f s: %.2f ms/step, %.0fx real time, %.0f MiB resident\n", config.nx, config.ny,
        config.nz, static_cast<double>(config.nx) * config.ny * config.nz / 1.0e6, steps, static_cast<double>(dt), perStep,
        dt / (perStep / 1000.0), static_cast<double>(gpu.ResidentBytes()) / (1024.0 * 1024.0));
    FastStormSolver downloaded(config);
    gpu.Download(downloaded);
    const FastStormState s = downloaded.MutableState();
    float wmax = 0.0F;
    const std::size_t n = static_cast<std::size_t>(config.nx) * config.ny * (config.nz + 1U);
    for (std::size_t i = 0; i < n; ++i) { wmax = std::max(wmax, s.w[i]); }
    std::printf("after %.0f s: max updraft %.1f m/s, max divergence %.2e\n", gpu.Time(), static_cast<double>(wmax),
        MaxDivergence(downloaded));
    return 0;
}
int NestBench(const Options& o)
{
    auto device = OpenDevice();
    if (!device) { return 3; }
    shader::dxc::DxcShaderCompiler compiler;
    weather_gpu::NestedGpuConfig nest;
    nest.parent = ConfigFrom(o, 128.0, 12.0);
    nest.ratio = static_cast<std::uint32_t>(Number(o, "ratio", 4.0));
    nest.cells = static_cast<std::uint32_t>(Number(o, "cells", nest.parent.nx / 4.0));
    nest.relaxCells = static_cast<std::uint32_t>(Number(o, "relax", 12.0));
    nest.childSubsteps = static_cast<std::uint32_t>(Number(o, "substeps", 2.0));
    if (const std::string problem = weather_gpu::NestedGpuSolver::Validate(nest); !problem.empty())
    {
        std::cerr << problem << '\n';
        return 2;
    }
    const auto steps = static_cast<std::uint32_t>(Number(o, "steps", 40.0));
    const float dt = nest.parent.maxTimeStep;
    FastStormSolver cpu(nest.parent);
    weather_gpu::NestedGpuSolver solver(*device, compiler, nest);
    solver.Upload(cpu);
    solver.Advance(2U, dt);
    double best = 1.0e30;
    for (int run = 0; run < 3; ++run)
    {
        const auto start = std::chrono::steady_clock::now();
        solver.Advance(steps, dt);
        best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    const double perStep = best / steps;
    const weather_lab::FastStormConfig& child = solver.Child().Config();
    std::printf("parent %ux%ux%u at %.0f m + child %ux%u at %.0f m (%u child steps per parent step): %.2f ms per parent step, %.0fx real time, %.0f MiB\n",
        nest.parent.nx, nest.parent.ny, nest.parent.nz, static_cast<double>(nest.parent.dx), child.nx, child.ny, static_cast<double>(child.dx),
        nest.childSubsteps, perStep, dt / (perStep / 1000.0), static_cast<double>(solver.ResidentBytes()) / (1024.0 * 1024.0));
    return 0;
}
int NestDiag(const Options& o)
{
    auto device = OpenDevice();
    if (!device) { return 3; }
    shader::dxc::DxcShaderCompiler compiler;
    weather_gpu::NestedGpuConfig nest;
    nest.parent = ConfigFrom(o, 128.0, 12.0);
    nest.parent.bubble.amplitude = static_cast<float>(Number(o, "bubble", 2.0));
    nest.ratio = 4;
    nest.cells = nest.parent.nx / 4;
    nest.relaxCells = static_cast<std::uint32_t>(Number(o, "relax", 12.0));
    const auto steps = static_cast<std::uint32_t>(Number(o, "steps", 150.0));
    const float dt = nest.parent.maxTimeStep;
    FastStormSolver cpu(nest.parent);
    weather_gpu::NestedGpuSolver solver(*device, compiler, nest);
    solver.Upload(cpu);
    solver.Advance(steps, dt);
    const weather_lab::FastStormConfig& cc = solver.Child().Config();
    FastStormSolver child(cc);
    solver.Child().Download(child);
    const FastStormState s = child.MutableState();
    const std::size_t plane = static_cast<std::size_t>(cc.nx) * cc.ny;
    // Per column: the highest level that has condensate and its peak (qc + qi + qs), and where those columns sit relative to the edge.
    std::vector<float> peak(plane, 0.0F);
    std::vector<int> topLevel(plane, -1);
    for (std::uint32_t k = 0; k < cc.nz; ++k)
    {
        for (std::size_t c = 0; c < plane; ++c)
        {
            const float q = s.qc[k * plane + c] + s.qi[k * plane + c] + s.qs[k * plane + c];
            if (q > peak[c]) { peak[c] = q; }
            if (q > 1.0e-6F) { topLevel[c] = static_cast<int>(k); }
        }
    }
    float global = 0.0F;
    for (const float v : peak) { global = std::max(global, v); }
    std::printf("child %ux%u, condensate peak per column (g/kg), every 4th cell; * marks the %u cell relaxation band:\n", cc.nx, cc.ny, nest.relaxCells);
    for (std::uint32_t j = cc.ny; j-- > 0;)
    {
        if (j % 4U != 0U) { continue; }
        for (std::uint32_t i = 0; i < cc.nx; i += 2U)
        {
            const float v = peak[j * cc.nx + i] * 1000.0F;
            const bool band = i < nest.relaxCells || i >= cc.nx - nest.relaxCells || j < nest.relaxCells || j >= cc.ny - nest.relaxCells;
            char ch = v < 0.002F ? '.' : (v < 0.05F ? ':' : (v < 0.5F ? 'o' : (v < 2.0F ? 'O' : '#')));
            if (band && ch != '.') { ch = ch == '#' ? '@' : '*'; }
            std::putchar(ch);
        }
        std::putchar('\n');
    }
    {
        // The child's edge column against the parent's value at the same place, by height.
        FastStormSolver parentBack(nest.parent);
        solver.Parent().Download(parentBack);
        const FastStormState ps = parentBack.MutableState();
        const std::uint32_t originI = (nest.parent.nx - nest.cells) / 2U;
        const std::uint32_t originJ = (nest.parent.ny - nest.cells) / 2U;
        const std::size_t pPlane = static_cast<std::size_t>(nest.parent.nx) * nest.parent.ny;
        const std::uint32_t j = cc.ny / 2U;
        std::printf("row j=%u: child i=0..3 and parent column at the child's west edge, condensate g/kg by level (k: child0 child1 child4 parent)\n", j);
        for (std::uint32_t k = 0; k < cc.nz; k += 3U)
        {
            const auto edge = [&](const std::uint32_t i) { return (s.qc[(k * cc.ny + j) * cc.nx + i] + s.qi[(k * cc.ny + j) * cc.nx + i] + s.qs[(k * cc.ny + j) * cc.nx + i]) * 1000.0F; };
            const std::size_t pi = k * pPlane + (originJ + j / 4U) * nest.parent.nx + originI;
            const float parent = (ps.qc[pi] + ps.qi[pi] + ps.qs[pi]) * 1000.0F;
            const float parentOutside = (ps.qc[pi - 1U] + ps.qi[pi - 1U] + ps.qs[pi - 1U]) * 1000.0F;
            std::printf("  k=%2u  %.4f %.4f %.4f | parent at edge %.4f, one cell outside %.4f\n", k, static_cast<double>(edge(0)), static_cast<double>(edge(1)),
                static_cast<double>(edge(4)), static_cast<double>(parent), static_cast<double>(parentOutside));
        }
    }
    // Condensate sitting in the band and the interior, by column count.
    int bandColumns = 0, interiorColumns = 0;
    for (std::uint32_t j = 0; j < cc.ny; ++j)
    {
        for (std::uint32_t i = 0; i < cc.nx; ++i)
        {
            if (peak[j * cc.nx + i] * 1000.0F < 0.002F) { continue; }
            const bool band = i < nest.relaxCells || i >= cc.nx - nest.relaxCells || j < nest.relaxCells || j >= cc.ny - nest.relaxCells;
            (band ? bandColumns : interiorColumns) += 1;
        }
    }
    std::printf("columns with condensate: %d in the relaxation band, %d in the interior (global peak %.3f g/kg)\n", bandColumns, interiorColumns,
        static_cast<double>(global) * 1000.0);
    return 0;
}
} // namespace

int main(const int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: orbit_weather_gpu compare|bench [--option value ...]\n";
        return 2;
    }
    const std::string command = argv[1];
    if (command == "compare") { return Compare(ParseOptions(argc, argv, 2)); }
    if (command == "bench") { return Bench(ParseOptions(argc, argv, 2)); }
    if (command == "nest") { return NestBench(ParseOptions(argc, argv, 2)); }
    if (command == "nestdiag") { return NestDiag(ParseOptions(argc, argv, 2)); }
    std::cerr << "unknown command\n";
    return 2;
}

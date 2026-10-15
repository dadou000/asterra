// orbit_weather_lab: command line front end for the SC-01 storm experiments.
//
//   orbit_weather_lab run --out fast.orbitwx --minutes 120 [options]
//   orbit_weather_lab nest --out child.orbitwx [--parent-out parent.orbitwx] --ratio 4 --cells 32 [--feedback 1] [run options]
//   orbit_weather_lab metrics run.orbitwx
//   orbit_weather_lab compare reference.orbitwx candidate.orbitwx
//   orbit_weather_lab budget [run options]

#include <orbit/weather_lab/FastStormSolver.hpp>
#include <orbit/weather_lab/NestedStormSolver.hpp>
#include <orbit/weather_lab/StormMetrics.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>

using namespace orbit::weather_lab;

namespace
{
using Options = std::map<std::string, std::string>;

Options ParseOptions(const int argc, char** argv, const int first)
{
    Options options;
    for (int i = first; i < argc; ++i)
    {
        const std::string key = argv[i];
        if (key.rfind("--", 0) == 0 && i + 1 < argc)
        {
            options[key.substr(2)] = argv[++i];
        }
    }
    return options;
}

double Number(const Options& o, const char* key, const double fallback)
{
    const auto it = o.find(key);
    return it == o.end() ? fallback : std::atof(it->second.c_str());
}

FastStormConfig ConfigFrom(const Options& o)
{
    FastStormConfig c;
    c.nx = static_cast<std::uint32_t>(Number(o, "nx", c.nx));
    c.ny = static_cast<std::uint32_t>(Number(o, "ny", c.ny));
    c.nz = static_cast<std::uint32_t>(Number(o, "nz", c.nz));
    c.dx = static_cast<float>(Number(o, "dx", c.dx));
    c.dy = static_cast<float>(Number(o, "dy", c.dx));
    c.dz = static_cast<float>(Number(o, "dz", c.dz));
    c.maxTimeStep = static_cast<float>(Number(o, "dt", c.maxTimeStep));
    c.maxCourant = static_cast<float>(Number(o, "courant", c.maxCourant));
    c.bubble.amplitude = static_cast<float>(Number(o, "bubble", c.bubble.amplitude));
    c.moisture = Number(o, "moisture", 1.0) != 0.0;
    c.ice = Number(o, "ice", 0.0) != 0.0;
    c.lateralSpongeCells = static_cast<std::uint32_t>(Number(o, "sponge", 0.0));
    c.lateralSpongeRate = static_cast<float>(Number(o, "sponge-rate", c.lateralSpongeRate));
    c.trackStorm = Number(o, "track", 0.0) != 0.0;
    c.trackSeconds = static_cast<float>(Number(o, "track-seconds", c.trackSeconds));
    c.massFixer = Number(o, "mass-fixer", 1.0) != 0.0;
    c.horizontalMixing = static_cast<float>(Number(o, "kh", c.horizontalMixing));
    c.verticalMixing = static_cast<float>(Number(o, "kv", c.verticalMixing));
    c.threads = static_cast<std::uint32_t>(Number(o, "threads", 0.0));
    const auto adv = o.find("advection");
    if (adv != o.end() && adv->second == "linear")
    {
        c.advection = AdvectionScheme::Linear;
    }
    return c;
}

int Run(const Options& o)
{
    const FastStormConfig config = ConfigFrom(o);
    if (const std::string problem = FastStormSolver::Validate(config);
        !problem.empty())
    {
        std::cerr << "invalid configuration: " << problem << '\n';
        return 2;
    }
    const double minutes = Number(o, "minutes", 120.0);
    const double every = Number(o, "every", 600.0);
    const auto outIt = o.find("out");

    FastStormSolver solver(config);
    WxWriter writer;
    std::vector<float> times;
    const auto frames = static_cast<std::size_t>(minutes * 60.0 / every) + 1U;
    for (std::size_t f = 0; f < frames; ++f)
    {
        times.push_back(static_cast<float>(static_cast<double>(f) * every));
    }
    if (outIt != o.end())
    {
        std::string error;
        if (!writer.Open(outIt->second, solver.MakeHeader(times), &error))
        {
            std::cerr << error << '\n';
            return 2;
        }
        (void)solver.WriteFrame(writer);
    }

    std::printf("fast core: %ux%ux%u cells (%.0f m x %.0f m), %.1f MiB resident\n",
        config.nx, config.ny, config.nz,
        static_cast<double>(config.dx), static_cast<double>(config.dz),
        static_cast<double>(solver.ResidentBytes()) / (1024.0 * 1024.0));
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t f = 1; f < frames; ++f)
    {
        solver.Advance(every);
        if (outIt != o.end())
        {
            (void)solver.WriteFrame(writer);
        }
        const auto& d = solver.Diagnostics();
        std::printf("t=%6.1f min  wmax=%5.1f  wmin=%6.1f  steps=%llu  dt=%4.1f  "
                    "div=%.1e  water drift=%+.2e\n",
            d.time / 60.0, static_cast<double>(d.maxUpdraft),
            static_cast<double>(d.maxDowndraft),
            static_cast<unsigned long long>(d.steps),
            static_cast<double>(d.lastTimeStep),
            static_cast<double>(d.maxDivergence),
            (d.totalWater - d.initialTotalWater) / d.initialTotalWater);
        std::fflush(stdout);
    }
    writer.Close();
    const double wall = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    const auto& d = solver.Diagnostics();
    std::printf("simulated %.0f s in %.2f s wall: %.1fx real time (%llu steps, "
                "%.2f ms/step)\n",
        d.time, wall, d.time / wall,
        static_cast<unsigned long long>(d.steps),
        wall * 1000.0 / static_cast<double>(std::max<std::uint64_t>(d.steps, 1U)));
    std::printf("last step ms: advect %.2f  micro %.2f  forcing %.2f  project %.2f\n",
        d.timings.advectMs, d.timings.microphysicsMs, d.timings.forcingMs,
        d.timings.projectionMs);
    return 0;
}

int Nest(const Options& o)
{
    NestConfig nc;
    nc.parent = ConfigFrom(o);
    nc.ratio = static_cast<std::uint32_t>(Number(o, "ratio", nc.ratio));
    nc.cells = static_cast<std::uint32_t>(Number(o, "cells", nc.cells));
    nc.relaxCells = static_cast<std::uint32_t>(Number(o, "relax-cells", nc.relaxCells));
    nc.relaxSeconds = static_cast<float>(Number(o, "relax-seconds", nc.relaxSeconds));
    nc.feedback = Number(o, "feedback", 0.0) != 0.0;
    nc.feedbackSeconds = static_cast<float>(Number(o, "feedback-seconds", nc.feedbackSeconds));
    nc.adaptive = Number(o, "adaptive", 0.0) != 0.0;
    nc.createUpdraft = static_cast<float>(Number(o, "create-updraft", nc.createUpdraft));
    nc.releaseUpdraft = static_cast<float>(Number(o, "release-updraft", nc.releaseUpdraft));
    nc.releaseHoldSeconds = static_cast<float>(Number(o, "release-hold", nc.releaseHoldSeconds));
    nc.maxChildCells = static_cast<std::uint32_t>(Number(o, "max-child-cells", 0.0));
    if (const std::string problem = NestedStormSolver::Validate(nc); !problem.empty())
    {
        std::cerr << "invalid nest: " << problem << '\n';
        return 2;
    }
    const double minutes = Number(o, "minutes", 120.0);
    const double every = Number(o, "every", 600.0);
    const auto frames = static_cast<std::size_t>(minutes * 60.0 / every) + 1U;
    std::vector<float> times;
    for (std::size_t f = 0; f < frames; ++f)
    {
        times.push_back(static_cast<float>(static_cast<double>(f) * every));
    }
    NestedStormSolver nest(nc);
    WxWriter childWriter;
    WxWriter parentWriter;
    const auto childOut = o.find("out");
    const auto parentOut = o.find("parent-out");
    std::string error;
    bool childOpen = false;
    if (childOut != o.end() && nest.HasChild())
    {
        if (!childWriter.Open(childOut->second, nest.Child().MakeHeader(times), &error))
        {
            std::cerr << error << '\n';
            return 2;
        }
        childOpen = true;
    }
    if (parentOut != o.end() && !parentWriter.Open(parentOut->second, nest.Parent().MakeHeader(times), &error))
    {
        std::cerr << error << '\n';
        return 2;
    }
    const auto write = [&]
    {
        if (childOpen && nest.HasChild()) { (void)nest.Child().WriteFrame(childWriter); }
        if (parentOut != o.end()) { (void)nest.Parent().WriteFrame(parentWriter); }
    };
    write();
    const FastStormConfig& pc = nc.parent;
    std::printf("nest: parent %ux%ux%u at %.0f m (%.0f km square)%s%s, child %ux%u cells of parent at 1/%u, %s%s\n",
        pc.nx, pc.ny, pc.nz, static_cast<double>(pc.dx), pc.nx * static_cast<double>(pc.dx) / 1000.0,
        pc.lateralSpongeCells > 0U ? ", lateral sponge" : "", pc.trackStorm ? ", storm-following frame" : "",
        nc.cells, nc.cells, nc.ratio, nc.feedback ? "two-way" : "one-way", nc.adaptive ? ", adaptive" : "");
    // Condensate that has reached the upstream and the downstream quarter of the domain (a wrapping outflow shows up upstream).
    const auto strips = [&](float& upstream, float& downstream)
    {
        std::vector<float> total(static_cast<std::size_t>(pc.nx) * pc.ny * pc.nz, 0.0F);
        std::vector<float> field;
        for (const char* name : {"qc", "qr", "qi", "qs", "qg"})
        {
            nest.Parent().CopyField(name, field);
            for (std::size_t i = 0; i < total.size(); ++i) { total[i] += field[i]; }
        }
        upstream = 0.0F;
        downstream = 0.0F;
        for (std::size_t i = 0; i < total.size(); ++i)
        {
            const std::uint32_t x = static_cast<std::uint32_t>(i % pc.nx);
            if (x < pc.nx / 4U) { upstream = std::max(upstream, total[i]); }
            if (x >= pc.nx * 3U / 4U) { downstream = std::max(downstream, total[i]); }
        }
        upstream *= 1000.0F;
        downstream *= 1000.0F;
    };
    const auto start = std::chrono::steady_clock::now();
    std::uint64_t cellStepSum = 0;
    std::uint64_t stepsTaken = 0;
    for (std::size_t f = 1; f < frames; ++f)
    {
        const std::uint64_t stepsBefore = nest.Parent().Diagnostics().steps;
        nest.Advance(every);
        const std::uint64_t newSteps = nest.Parent().Diagnostics().steps - stepsBefore;
        cellStepSum += nest.TotalCells() * newSteps;
        stepsTaken += newSteps;
        if (!childOpen && childOut != o.end() && nest.HasChild())
        {
            (void)childWriter.Open(childOut->second, nest.Child().MakeHeader(times), &error);
            childOpen = true;
        }
        write();
        const auto& dp = nest.Parent().Diagnostics();
        float upstream = 0.0F, downstream = 0.0F;
        strips(upstream, downstream);
        std::printf("t=%6.1f min  wmax=%5.1f  frame=(%5.1f,%5.1f)  child=%s%u  cells=%.2fM  cond up/down [g/kg]=%.3f/%.3f  div=%.1e\n",
            dp.time / 60.0, static_cast<double>(dp.maxUpdraft), static_cast<double>(nest.Parent().Config().frameU),
            static_cast<double>(nest.Parent().Config().frameV), nest.HasChild() ? "" : "none ",
            nest.HasChild() ? nest.ChildCells() : 0U, static_cast<double>(nest.TotalCells()) / 1.0e6,
            static_cast<double>(upstream), static_cast<double>(downstream), static_cast<double>(dp.maxDivergence));
        std::fflush(stdout);
    }
    childWriter.Close();
    parentWriter.Close();
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("simulated %.0f s in %.2f s wall: %.1fx real time; mean %.2f M cells per step; child created %u time(s), "
                "budget coarsened it %u time(s)\n",
        nest.Parent().Diagnostics().time, wall, nest.Parent().Diagnostics().time / wall,
        static_cast<double>(cellStepSum) / static_cast<double>(std::max<std::uint64_t>(stepsTaken, 1U)) / 1.0e6,
        nest.ChildCreations(), nest.BudgetCoarsenings());
    return 0;
}

int Metrics(const std::string& path)
{
    WxReader reader;
    std::string error;
    std::vector<StormMetrics> metrics;
    if (!reader.Open(path, &error) || !ComputeStormMetrics(reader, metrics, &error))
    {
        std::cerr << error << '\n';
        return 2;
    }
    std::printf("%s (%s, %ux%ux%u, %.0f m)\n%s", path.c_str(),
        reader.Header().source.c_str(), reader.Header().nx, reader.Header().ny,
        reader.Header().nz, static_cast<double>(reader.Header().dx),
        FormatMetricsTable(metrics).c_str());
    return 0;
}

int Compare(const std::string& a, const std::string& b)
{
    WxReader ra;
    WxReader rb;
    std::string error;
    std::vector<StormMetrics> ma;
    std::vector<StormMetrics> mb;
    if (!ra.Open(a, &error) || !rb.Open(b, &error)
        || !ComputeStormMetrics(ra, ma, &error)
        || !ComputeStormMetrics(rb, mb, &error))
    {
        std::cerr << error << '\n';
        return 2;
    }
    std::printf("reference: %s [%s]\ncandidate: %s [%s]\n\n", a.c_str(),
        ra.Header().source.c_str(), b.c_str(), rb.Header().source.c_str());
    std::printf("   t[min] |        wmax ref/cand |  UH2-5 ref/cand |"
                " zeta<1km ref/cand | coldpool ref/cand | top[km] ref/cand\n");
    for (const StormMetrics& r : ma)
    {
        const StormMetrics* match = nullptr;
        for (const StormMetrics& c : mb)
        {
            if (std::fabs(c.time - r.time) < 1.0F) { match = &c; }
        }
        if (match == nullptr) { continue; }
        std::printf("%9.1f | %9.1f %9.1f | %7.0f %7.0f | %8.4f %8.4f | %7.2f %7.2f | %6.1f %6.1f\n",
            static_cast<double>(r.time) / 60.0,
            static_cast<double>(r.maxUpdraft), static_cast<double>(match->maxUpdraft),
            static_cast<double>(r.maxUpdraftHelicity), static_cast<double>(match->maxUpdraftHelicity),
            static_cast<double>(r.maxLowVorticity), static_cast<double>(match->maxLowVorticity),
            static_cast<double>(r.coldPoolDeficit), static_cast<double>(match->coldPoolDeficit),
            static_cast<double>(r.cloudTop) / 1000.0, static_cast<double>(match->cloudTop) / 1000.0);
    }
    return 0;
}

int Budget(const Options& o)
{
    // Memory the fast core would need at other domain sizes, against the 2 GB
    // minimum weather allocation.
    FastStormConfig c = ConfigFrom(o);
    FastStormSolver solver(c);
    const double perCell = static_cast<double>(solver.ResidentBytes())
        / (static_cast<double>(c.nx) * c.ny * c.nz);
    std::printf("resident bytes per cell: %.1f (CPU reference, fp32 + fp64 spectrum)\n", perCell);
    std::printf("2 GB holds about %.1f M cells at this layout\n",
        2.0 * 1024.0 * 1024.0 * 1024.0 * 0.7 / perCell / 1.0e6);
    return 0;
}
} // namespace

int main(const int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: orbit_weather_lab run|metrics|compare|budget ...\n";
        return 2;
    }
    const std::string command = argv[1];
    if (command == "run") { return Run(ParseOptions(argc, argv, 2)); }
    if (command == "nest") { return Nest(ParseOptions(argc, argv, 2)); }
    if (command == "budget") { return Budget(ParseOptions(argc, argv, 2)); }
    if (command == "metrics" && argc >= 3) { return Metrics(argv[2]); }
    if (command == "compare" && argc >= 4) { return Compare(argv[2], argv[3]); }
    std::cerr << "unknown command or missing arguments\n";
    return 2;
}

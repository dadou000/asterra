#include <orbit/studio_ui/StudioViewportRenderer.hpp>

#include <orbit/core/Log.hpp>

#include <stdexcept>
#include <string>
#include <orbit/weather_gpu/GpuStormSolver.hpp>
#include <orbit/weather_lab/FastStormSolver.hpp>

#include <algorithm>
#include <cmath>
#include <memory>

namespace orbit::studio_ui
{
namespace
{
constexpr f64 kPi = 3.14159265358979323846;
constexpr u32 kMaxStepsPerFrame = 8U;
constexpr f64 kJumpSeconds = 3600.0;   // a larger gap in simulation time restarts the storm instead of catching up
} // namespace

// The simulated storm domain of the primary view (docs: /world/weather-lab/gpu-port and global-system-plan). Created, stepped and
// exported on the render thread only, because the solver submits to the Vulkan queue.
struct StudioViewportRenderer::StormRuntime
{
    StormPlaceRequest request;
    weather_lab::FastStormConfig config;
    std::unique_ptr<weather_gpu::GpuStormSolver> solver;
    universe::BodyId body{};
    f64 radiusMeters{6371000.0};
    f64 placedSimSeconds{0.0};   // simulation time of the placement
    f64 spinSeconds{0.0};
    f32 stepSeconds{24.0F};
    f64 lastSimSeconds{0.0};
    u64 steps{0};
    f64 stepMilliseconds{0.0};
    bool exportedOnce{false};
    StormStatus status;
};

StudioViewportRenderer::~StudioViewportRenderer() = default;

void StudioViewportRenderer::UpdateStormDomain(
    const time::SimulationTime now, const universe::BodyId& body, const f64 planetRadiusMeters)
{
    if (!storm_)
    {
        storm_ = std::make_shared<StormRuntime>();
    }
    StormRuntime& rt = *storm_;
    const f64 nowSeconds = static_cast<f64>(now.microsecondsFromEpoch) * 1.0e-6;

    if (stormMailbox_.TakeClear())
    {
        rt.solver.reset();
        rt.status = {};
        cloudRenderer_.SetStormBuffer(nullptr);
    }

    const auto build = [&](const StormPlaceRequest& request)
    {
        rt.status = {};
        rt.status.latitudeDegrees = request.latitudeDegrees;
        rt.status.longitudeDegrees = request.longitudeDegrees;
        rt.status.cells = request.cells;
        rt.status.spacingMeters = request.spacingMeters;
        try
        {
            weather_lab::FastStormConfig config;
            config.nx = request.cells;
            config.ny = request.cells;
            config.nz = 40U;
            config.dx = request.spacingMeters;
            config.dy = request.spacingMeters;
            config.dz = 500.0F;
            config.ice = request.ice;
            config.bubble.amplitude = request.bubbleKelvin;
            config.lateralSpongeCells = request.spongeCells;
            config.threads = 4U;
            config.maxTimeStep = std::clamp(request.spacingMeters / 2000.0F * 24.0F, 6.0F, 24.0F);
            config.maxCourant = 1.0e9F;
            if (const std::string problem = weather_gpu::GpuStormSolver::Validate(config); !problem.empty())
            {
                throw std::runtime_error(problem);
            }
            weather_lab::FastStormSolver cpu(config);
            auto solver = std::make_unique<weather_gpu::GpuStormSolver>(*device_, *compiler_, config);
            solver->Upload(cpu);
            rt.stepSeconds = config.maxTimeStep;
            rt.spinSeconds = static_cast<f64>(request.spinMinutes) * 60.0;
            const auto spinSteps = static_cast<u32>(rt.spinSeconds / rt.stepSeconds);
            for (u32 done = 0; done < spinSteps; done += 50U)
            {
                solver->Advance(std::min(50U, spinSteps - done), rt.stepSeconds);
            }
            rt.request = request;
            rt.config = config;
            rt.solver = std::move(solver);
            rt.body = body;
            rt.radiusMeters = planetRadiusMeters;
            rt.placedSimSeconds = nowSeconds;
            rt.lastSimSeconds = nowSeconds;
            rt.steps = spinSteps;
            rt.exportedOnce = false;
            rt.status.state = StormStatus::State::Active;
            rt.status.message = "running";
        }
        catch (const std::exception& exception)
        {
            rt.solver.reset();
            rt.status.state = StormStatus::State::Failed;
            rt.status.message = exception.what();
            log::Warning(std::string("Storm domain placement failed: ") + exception.what());
        }
    };

    if (const auto request = stormMailbox_.TakePlace(); request.has_value())
    {
        build(*request);
    }

    if (!rt.solver || rt.body != body)
    {
        cloudRenderer_.SetStormBuffer(nullptr);
        if (rt.status.state != StormStatus::State::None)
        {
            stormMailbox_.Publish(rt.status);
        }
        return;
    }

    // Step in lockstep with the simulation clock. A clock that went backwards or jumped far ahead restarts the storm: catching up
    // across a timewarp is phase 6 (checkpoints), not done here.
    const f64 target = rt.spinSeconds + (nowSeconds - rt.placedSimSeconds);
    if (nowSeconds < rt.lastSimSeconds - 1.0 || target - rt.solver->Time() > kJumpSeconds)
    {
        build(rt.request);
        if (!rt.solver) { return; }
    }
    else
    {
        const f64 behind = target - rt.solver->Time();
        const auto steps = std::min(kMaxStepsPerFrame, static_cast<u32>(std::max(0.0, behind / rt.stepSeconds)));
        if (steps > 0U)
        {
            rt.solver->Advance(steps, rt.stepSeconds);
            rt.steps += steps;
            rt.stepMilliseconds = rt.solver->LastAdvanceMilliseconds() / steps;
        }
        if (steps > 0U || !rt.exportedOnce)
        {
            // The domain translates with the Galilean frame: its centre drifts at (frameU, frameV) over the ground.
            const f64 travelled = rt.solver->Time() - rt.spinSeconds;
            const f64 latitude0 = rt.request.latitudeDegrees * kPi / 180.0;
            const f64 longitude0 = rt.request.longitudeDegrees * kPi / 180.0;
            const f64 latitude = latitude0 + static_cast<f64>(rt.config.frameV) * travelled / rt.radiusMeters;
            const f64 longitude = longitude0
                + static_cast<f64>(rt.config.frameU) * travelled / (rt.radiusMeters * std::max(std::cos(latitude0), 0.05));
            const f64 cl = std::cos(latitude), sl = std::sin(latitude), co = std::cos(longitude), so = std::sin(longitude);
            weather_gpu::StormPlacement placement;
            placement.active = true;
            placement.planetRadius = static_cast<f32>(rt.radiusMeters);
            // Body-fixed frame of the cloud renderer: +Y is the pole, longitude grows toward -Z.
            placement.centre = {rt.radiusMeters * cl * co, rt.radiusMeters * sl, -rt.radiusMeters * cl * so};
            placement.east = {static_cast<f32>(-so), 0.0F, static_cast<f32>(-co)};
            placement.north = {static_cast<f32>(-sl * co), static_cast<f32>(cl), static_cast<f32>(sl * so)};
            rt.solver->ExportCloud(placement);
            rt.exportedOnce = true;
            rt.status.latitudeDegrees = latitude * 180.0 / kPi;
            rt.status.longitudeDegrees = longitude * 180.0 / kPi;
        }
    }
    rt.lastSimSeconds = nowSeconds;
    rt.status.state = StormStatus::State::Active;
    rt.status.message = "running";
    rt.status.stormSeconds = rt.solver->Time();
    rt.status.steps = rt.steps;
    rt.status.stepMilliseconds = rt.stepMilliseconds;
    rt.status.residentMiB = static_cast<f64>(rt.solver->ResidentBytes()) / (1024.0 * 1024.0);
    cloudRenderer_.SetStormBuffer(&rt.solver->CloudBuffer());
    stormMailbox_.Publish(rt.status);
}
} // namespace orbit::studio_ui

#include <orbit/studio_ui/StudioViewportRenderer.hpp>

#include <orbit/celestial_clouds/StormSchedule.hpp>
#include <orbit/core/Log.hpp>
#include <orbit/weather_gpu/GpuStormSolver.hpp>
#include <orbit/weather_gpu/NestedGpuSolver.hpp>
#include <orbit/weather_lab/FastStormSolver.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace orbit::studio_ui
{
namespace
{
constexpr f64 kPi = 3.14159265358979323846;
constexpr f64 kCheckpointCadenceSeconds = 1200.0;   // a checkpoint every 20 minutes of storm time
constexpr std::size_t kCheckpointBudgetBytes = 640ULL * 1024ULL * 1024ULL;
constexpr u32 kStepsPerFrame = 4U;                  // while keeping up with the clock: at most this many steps a frame
constexpr u32 kCatchUpStepsPerFrame = 24U;          // while behind (a time jump, a new storm): about 70 ms of GPU work a frame
constexpr f64 kFadeSeconds = 2400.0;                // the last 40 minutes of a storm dissolve into the procedural clouds
constexpr f64 kManualLifetimeSeconds = 1.0e15;

f64 SmoothStep(const f64 a, const f64 b, const f64 x)
{
    const f64 t = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// One storm's simulation: a single level, or a parent with a child grid. The manager drives both the same way.
class StormSim
{
public:
    StormSim(rhi::Device& device, const shader::Compiler& compiler, const weather_lab::FastStormConfig& config, const bool nested)
    {
        if (nested && config.nx >= 32U)
        {
            weather_gpu::NestedGpuConfig nest;
            nest.parent = config;
            nest.ratio = 4U;
            nest.cells = config.nx / 4U;
            nest.relaxCells = std::min(12U, config.nx / 4U);
            nest.relaxSeconds = 10.0F;
            nest.childSubsteps = 2U;
            if (const std::string problem = weather_gpu::NestedGpuSolver::Validate(nest); !problem.empty())
            {
                throw std::runtime_error(problem);
            }
            nested_ = std::make_unique<weather_gpu::NestedGpuSolver>(device, compiler, nest);
            childSpacing_ = config.dx / 4.0F;
        }
        else
        {
            single_ = std::make_unique<weather_gpu::GpuStormSolver>(device, compiler, config);
        }
    }
    void Upload(weather_lab::FastStormSolver& cpu) { if (nested_) { nested_->Upload(cpu); } else { single_->Upload(cpu); } }
    void Advance(const u32 steps, const f32 dt) { if (nested_) { nested_->Advance(steps, dt); } else { single_->Advance(steps, dt); } }
    [[nodiscard]] weather_gpu::StormCheckpoint Save() { return nested_ ? nested_->Save() : single_->Save(); }
    void Restore(const weather_gpu::StormCheckpoint& c) { if (nested_) { nested_->Restore(c); } else { single_->Restore(c); } }
    void ExportCloud(const weather_gpu::StormPlacement& p) { if (nested_) { nested_->ExportCloud(p); } else { single_->ExportCloud(p); } }
    [[nodiscard]] rhi::Buffer& CloudBuffer() { return nested_ ? nested_->CloudBuffer() : single_->CloudBuffer(); }
    [[nodiscard]] double Time() const { return nested_ ? nested_->Time() : single_->Time(); }
    [[nodiscard]] double LastAdvanceMilliseconds() const { return nested_ ? nested_->LastAdvanceMilliseconds() : single_->LastAdvanceMilliseconds(); }
    [[nodiscard]] std::size_t ResidentBytes() const { return nested_ ? nested_->ResidentBytes() : single_->ResidentBytes(); }
    [[nodiscard]] bool Nested() const { return nested_ != nullptr; }
    [[nodiscard]] f32 ChildSpacing() const { return childSpacing_; }

private:
    std::unique_ptr<weather_gpu::GpuStormSolver> single_;
    std::unique_ptr<weather_gpu::NestedGpuSolver> nested_;
    f32 childSpacing_{0.0F};
};

u64 Mix64(u64 x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31U);
}
} // namespace

// The simulated storm of the primary view (docs: /world/weather-lab/gpu-port and global-system-phases). Everything here runs on the render
// thread, because the solver submits to the Vulkan queue.
//
// A storm is described by (start, lifetime, place); what is drawn at simulation time t is the solver state at storm age t - start. The
// solver is stepped forward in lockstep with the clock; when the clock jumps (or a different storm is chosen) the state is restored from the
// nearest cached checkpoint at or before the wanted age and stepped from there, so a jump costs at most one checkpoint interval of steps
// when the storm was seen before, and one run from its beginning (spread over frames, nothing drawn meanwhile) when not.
struct StudioViewportRenderer::StormRuntime
{
    struct Active
    {
        bool valid{false};
        bool manual{false};
        u64 id{0};
        f64 startSeconds{0.0};
        f64 lifetimeSeconds{0.0};
        f64 latitudeRadians{0.0};
        f64 longitudeRadians{0.0};
        weather_lab::FastStormConfig config;
        bool nested{false};
    };

    StormPlaceRequest manualRequest;
    bool manualActive{false};
    f64 manualStartSeconds{0.0};
    u64 manualCounter{0};
    StormAutoRequest autoRequest;
    universe::BodyId body{};
    bool bodySet{false};

    Active active;
    std::unique_ptr<StormSim> solver;
    std::unique_ptr<weather_lab::FastStormSolver> cpuInit;
    u64 solverGridKey{0};
    u64 initId{~0ULL};
    bool haveState{false};
    f32 stepSeconds{24.0F};
    u64 steps{0};
    f64 stepMilliseconds{0.0};
    bool exported{false};

    struct Cached
    {
        f64 age;
        weather_gpu::StormCheckpoint data;
    };
    std::map<u64, std::vector<Cached>> checkpoints;
    std::map<u64, u64> lastUse;
    u64 useCounter{0};
    std::size_t checkpointBytes{0};
};

StudioViewportRenderer::~StudioViewportRenderer() = default;

void StudioViewportRenderer::UpdateStormDomain(
    const time::SimulationTime now,
    const universe::BodyId& body,
    const f64 planetRadiusMeters,
    const math::Double3& cameraBodyMeters,
    const math::Float3& sunDirectionBody,
    const celestial_clouds::CloudLayerParameters& layer)
{
    if (!storm_)
    {
        storm_ = std::make_shared<StormRuntime>();
    }
    StormRuntime& rt = *storm_;
    const f64 nowSeconds = static_cast<f64>(now.microsecondsFromEpoch) * 1.0e-6;

    if (const auto request = stormMailbox_.TakeAuto(); request.has_value())
    {
        rt.autoRequest = *request;
        rt.active.valid = false;
        rt.bodySet = false;
    }
    if (stormMailbox_.TakeClear())
    {
        rt.manualActive = false;
        rt.active.valid = false;
        rt.haveState = false;
    }
    if (const auto request = stormMailbox_.TakePlace(); request.has_value())
    {
        rt.manualRequest = *request;
        rt.manualActive = true;
        // The storm began `spin` minutes before the placement, so it is already developed when it first draws.
        rt.manualStartSeconds = nowSeconds - static_cast<f64>(request->spinMinutes) * 60.0;
        rt.active.valid = false;
        rt.haveState = false;
        rt.bodySet = false;
        ++rt.manualCounter;
    }
    if (!rt.bodySet)
    {
        rt.body = body;
        rt.bodySet = true;
    }

    // ---- which storm?
    StormRuntime::Active wanted;
    u32 scheduledNearby = 0;
    const f64 cameraLength = std::sqrt(static_cast<f64>(cameraBodyMeters.x) * cameraBodyMeters.x
        + static_cast<f64>(cameraBodyMeters.y) * cameraBodyMeters.y + static_cast<f64>(cameraBodyMeters.z) * cameraBodyMeters.z);
    const char* mode = "off";
    if (rt.manualActive)
    {
        mode = "manual";
        const StormPlaceRequest& m = rt.manualRequest;
        wanted.valid = true;
        wanted.manual = true;
        wanted.id = Mix64(0x4D414E55414CULL ^ rt.manualCounter);
        wanted.startSeconds = rt.manualStartSeconds;
        wanted.lifetimeSeconds = kManualLifetimeSeconds;
        wanted.latitudeRadians = m.latitudeDegrees * kPi / 180.0;
        wanted.longitudeRadians = m.longitudeDegrees * kPi / 180.0;
        wanted.config.nx = m.cells;
        wanted.config.ny = m.cells;
        wanted.config.dx = m.spacingMeters;
        wanted.config.dy = m.spacingMeters;
        wanted.config.ice = m.ice;
        wanted.config.bubble.amplitude = m.bubbleKelvin;
        wanted.config.lateralSpongeCells = m.spongeCells;
        wanted.nested = m.nested;
    }
    else if (rt.autoRequest.enabled && cameraLength > 1.0)
    {
        mode = "auto";
        celestial_clouds::StormScheduleConfig schedule;
        schedule.seed = rt.autoRequest.seed;
        celestial_clouds::WeatherParameters weather;
        weather.subsolarLatitudeRadians = std::asin(std::clamp(static_cast<f64>(sunDirectionBody.y), -1.0, 1.0));
        weather.noiseScale = layer.weatherScale;
        weather.detailScale = layer.detailScale;
        weather.seed = layer.seed;
        const f64 latitude = std::asin(std::clamp(static_cast<f64>(cameraBodyMeters.y) / cameraLength, -1.0, 1.0));
        const f64 longitude = std::atan2(-static_cast<f64>(cameraBodyMeters.z), static_cast<f64>(cameraBodyMeters.x));
        const auto near = celestial_clouds::StormsNear(latitude, longitude, static_cast<f64>(rt.autoRequest.searchKilometres) * 1000.0,
            planetRadiusMeters, nowSeconds, schedule, weather, celestial_clouds::WeatherClimate{});
        scheduledNearby = static_cast<u32>(near.size());
        if (!near.empty())
        {
            const celestial_clouds::ScheduledStorm& s = near.front();
            wanted.valid = true;
            wanted.id = s.id;
            wanted.startSeconds = s.startSeconds;
            wanted.lifetimeSeconds = s.lifetimeSeconds;
            wanted.latitudeRadians = s.latitudeRadians;
            wanted.longitudeRadians = s.longitudeRadians;
            wanted.config.nx = rt.autoRequest.cells;
            wanted.config.ny = rt.autoRequest.cells;
            wanted.config.dx = rt.autoRequest.spacingMeters;
            wanted.config.dy = rt.autoRequest.spacingMeters;
            wanted.config.ice = rt.autoRequest.ice;
            wanted.config.lateralSpongeCells = std::max(4U, rt.autoRequest.cells / 10U);
            wanted.nested = rt.autoRequest.nested;
            // Each storm gets its own warm bubble, so scheduled storms differ in strength.
            wanted.config.bubble.amplitude = 1.6F + 0.8F * static_cast<f32>(static_cast<f64>(Mix64(s.id) >> 11U) / 9007199254740992.0);
        }
    }

    StormStatus status;
    status.mode = mode;
    status.scheduledNearby = scheduledNearby;
    status.checkpointMiB = static_cast<f64>(rt.checkpointBytes) / (1024.0 * 1024.0);

    if (!wanted.valid || rt.body != body)
    {
        rt.active.valid = false;
        cloudRenderer_.SetStormBuffer(nullptr);
        if (rt.autoRequest.enabled && !rt.manualActive)
        {
            status.message = "no scheduled storm is alive near the camera";
        }
        stormMailbox_.Publish(status);
        return;
    }

    // ---- the solver for this storm's grid
    wanted.config.nz = 40U;
    wanted.config.dz = 500.0F;
    wanted.config.threads = 4U;
    wanted.config.maxTimeStep = std::clamp(wanted.config.dx / 2000.0F * 24.0F, 6.0F, 24.0F);
    wanted.config.maxCourant = 1.0e9F;
    if (wanted.config.lateralSpongeCells == 0U) { wanted.config.lateralSpongeCells = 12U; }
    try
    {
        const bool switched = !rt.active.valid || rt.active.id != wanted.id;
        const u64 gridKey = Mix64((static_cast<u64>(wanted.config.nx) << 32U) ^ static_cast<u64>(wanted.config.dx * 16.0F)
            ^ (wanted.config.ice ? 0x1000000000ULL : 0ULL) ^ (static_cast<u64>(wanted.config.lateralSpongeCells) << 48U)
            ^ (wanted.nested ? 0x7000000000000ULL : 0ULL));
        if (!rt.solver || rt.solverGridKey != gridKey)
        {
            if (const std::string problem = weather_gpu::GpuStormSolver::Validate(wanted.config); !problem.empty())
            {
                throw std::runtime_error(problem);
            }
            rt.solver = std::make_unique<StormSim>(*device_, *compiler_, wanted.config, wanted.nested);
            rt.solverGridKey = gridKey;
            rt.checkpoints.clear();
            rt.lastUse.clear();
            rt.checkpointBytes = 0;
            rt.haveState = false;
            rt.initId = ~0ULL;
        }
        if (switched)
        {
            rt.haveState = false;
            rt.exported = false;
        }
        rt.active = wanted;
        rt.stepSeconds = wanted.config.maxTimeStep;
        rt.lastUse[wanted.id] = ++rt.useCounter;
        const f64 age = std::clamp(nowSeconds - wanted.startSeconds, 0.0, wanted.lifetimeSeconds);

        // A different storm needs its own initial state (the bubble differs); the CPU solver only builds it.
        if (rt.initId != wanted.id)
        {
            rt.cpuInit = std::make_unique<weather_lab::FastStormSolver>(wanted.config);
            rt.initId = wanted.id;
        }

        // ---- restore when the state is not usable for this age: no state, or the clock went backwards
        if (!rt.haveState || age < rt.solver->Time() - 1.0)
        {
            const auto found = rt.checkpoints.find(wanted.id);
            const StormRuntime::Cached* best = nullptr;
            if (found != rt.checkpoints.end())
            {
                for (const auto& cached : found->second)
                {
                    if (cached.age <= age + 1.0e-6 && (best == nullptr || cached.age > best->age)) { best = &cached; }
                }
            }
            if (best != nullptr)
            {
                rt.solver->Restore(best->data);
            }
            else
            {
                rt.solver->Upload(*rt.cpuInit);
            }
            rt.haveState = true;
            rt.exported = false;
        }

        // ---- step toward the wanted age, saving a checkpoint at every cadence boundary passed
        const bool catchingUp = age - rt.solver->Time() > 3.0 * rt.stepSeconds;
        u32 budget = catchingUp ? kCatchUpStepsPerFrame : kStepsPerFrame;
        bool stepped = false;
        while (budget > 0U && age - rt.solver->Time() >= rt.stepSeconds)
        {
            const f64 time = rt.solver->Time();
            const f64 nextBoundary = (std::floor(time / kCheckpointCadenceSeconds + 1.0e-9) + 1.0) * kCheckpointCadenceSeconds;
            const auto toBoundary = static_cast<u32>(std::max(1.0, std::ceil((nextBoundary - time) / rt.stepSeconds - 1.0e-9)));
            const auto available = static_cast<u32>((age - time) / rt.stepSeconds);
            const u32 chunk = std::min({budget, available, toBoundary});
            if (chunk == 0U) { break; }
            rt.solver->Advance(chunk, rt.stepSeconds);
            rt.steps += chunk;
            rt.stepMilliseconds = rt.solver->LastAdvanceMilliseconds() / chunk;
            budget -= chunk;
            stepped = true;
            // Passed a boundary (within one step of it): save the state, once per boundary.
            const f64 reached = rt.solver->Time();
            const f64 boundary = std::floor((reached + 0.5 * rt.stepSeconds) / kCheckpointCadenceSeconds) * kCheckpointCadenceSeconds;
            auto& list = rt.checkpoints[wanted.id];
            const bool have = std::any_of(list.begin(), list.end(),
                [&](const StormRuntime::Cached& c) { return std::fabs(c.age - reached) < 0.5 * rt.stepSeconds; });
            if (boundary > 0.0 && std::fabs(reached - boundary) < 0.5 * rt.stepSeconds && !have)
            {
                weather_gpu::StormCheckpoint saved = rt.solver->Save();
                rt.checkpointBytes += saved.data.size() * sizeof(f32);
                const f64 savedAge = saved.time;
                list.push_back({savedAge, std::move(saved)});
                // Over budget: drop the least recently used other storm's checkpoints, then thin this storm's oldest.
                while (rt.checkpointBytes > kCheckpointBudgetBytes)
                {
                    u64 victim = 0;
                    u64 oldest = ~0ULL;
                    for (const auto& [id, use] : rt.lastUse)
                    {
                        const auto entry = rt.checkpoints.find(id);
                        if (id != wanted.id && entry != rt.checkpoints.end() && !entry->second.empty() && use < oldest)
                        {
                            oldest = use;
                            victim = id;
                        }
                    }
                    if (oldest == ~0ULL)
                    {
                        if (list.size() <= 1U) { break; }
                        rt.checkpointBytes -= list.front().data.data.size() * sizeof(f32);
                        list.erase(list.begin());
                        continue;
                    }
                    for (const auto& cached : rt.checkpoints[victim]) { rt.checkpointBytes -= cached.data.data.size() * sizeof(f32); }
                    rt.checkpoints.erase(victim);
                }
            }
        }
        const bool stillBehind = age - rt.solver->Time() > 3.0 * rt.stepSeconds;

        // ---- publish the domain to the cloud renderer (nothing while catching up)
        const f64 intensity = 1.0 - SmoothStep(wanted.lifetimeSeconds - kFadeSeconds, wanted.lifetimeSeconds, age);
        // The domain translates with the Galilean frame: its centre drifts at (frameU, frameV) over the ground.
        const f64 travelled = rt.solver->Time();
        const f64 latitude = wanted.latitudeRadians + static_cast<f64>(wanted.config.frameV) * travelled / planetRadiusMeters;
        const f64 longitude = wanted.longitudeRadians
            + static_cast<f64>(wanted.config.frameU) * travelled / (planetRadiusMeters * std::max(std::cos(wanted.latitudeRadians), 0.05));
        status.latitudeDegrees = latitude * 180.0 / kPi;
        status.longitudeDegrees = longitude * 180.0 / kPi;
        if (!stillBehind && intensity > 0.0)
        {
            if (stepped || !rt.exported)
            {
                const f64 cl = std::cos(latitude), sl = std::sin(latitude), co = std::cos(longitude), so = std::sin(longitude);
                weather_gpu::StormPlacement placement;
                placement.active = true;
                placement.planetRadius = static_cast<f32>(planetRadiusMeters);
                // Body-fixed frame of the cloud renderer: +Y is the pole, longitude grows toward -Z.
                placement.centre = {planetRadiusMeters * cl * co, planetRadiusMeters * sl, -planetRadiusMeters * cl * so};
                placement.east = {static_cast<f32>(-so), 0.0F, static_cast<f32>(-co)};
                placement.north = {static_cast<f32>(-sl * co), static_cast<f32>(cl), static_cast<f32>(sl * so)};
                placement.intensity = static_cast<f32>(intensity);
                rt.solver->ExportCloud(placement);
                rt.exported = true;
            }
            cloudRenderer_.SetStormBuffer(&rt.solver->CloudBuffer());
        }
        else
        {
            cloudRenderer_.SetStormBuffer(nullptr);
        }

        status.state = StormStatus::State::Active;
        status.message = stillBehind ? "catching up to the simulation clock" : "running";
        status.catchingUp = stillBehind;
        status.stormSeconds = rt.solver->Time();
        status.steps = rt.steps;
        status.stepMilliseconds = rt.stepMilliseconds;
        status.cells = wanted.config.nx;
        status.spacingMeters = wanted.config.dx;
        status.residentMiB = static_cast<f64>(rt.solver->ResidentBytes()) / (1024.0 * 1024.0);
        status.nested = rt.solver->Nested();
        status.childSpacingMeters = rt.solver->ChildSpacing();
        status.stormId = wanted.id;
        status.ageSeconds = age;
        status.lifetimeSeconds = wanted.manual ? 0.0 : wanted.lifetimeSeconds;
        status.intensity = intensity;
        status.checkpoints = static_cast<u32>(rt.checkpoints[wanted.id].size());
        status.checkpointMiB = static_cast<f64>(rt.checkpointBytes) / (1024.0 * 1024.0);
    }
    catch (const std::exception& exception)
    {
        rt.solver.reset();
        rt.active.valid = false;
        rt.haveState = false;
        cloudRenderer_.SetStormBuffer(nullptr);
        status.state = StormStatus::State::Failed;
        status.message = exception.what();
        log::Warning(std::string("Storm domain failed: ") + exception.what());
        // Do not retry every frame.
        rt.manualActive = false;
        rt.autoRequest.enabled = false;
    }
    stormMailbox_.Publish(status);
}
} // namespace orbit::studio_ui

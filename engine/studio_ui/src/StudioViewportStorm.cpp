#include <orbit/studio_ui/StudioViewportRenderer.hpp>

#include <orbit/celestial_clouds/StormSchedule.hpp>
#include <orbit/celestial_clouds/StormZoneRuntime.hpp>
#include <orbit/celestial_clouds/WeatherEvolution.hpp>
#include <orbit/celestial_clouds/WeatherEmergence.hpp>
#include <orbit/core/Log.hpp>
#include <orbit/weather_gpu/GpuStormSolver.hpp>
#include <orbit/weather_gpu/NestedGpuSolver.hpp>
#include <orbit/storm_electrification/StormLightningSource.hpp>
#include <orbit/weather_lab/FastStormSolver.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
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
    [[nodiscard]] std::string ReloadShaders(const std::string_view source) { return nested_ ? nested_->ReloadShaders(source) : single_->ReloadShaders(source); }
    void Upload(weather_lab::FastStormSolver& cpu) { if (nested_) { nested_->Upload(cpu); } else { single_->Upload(cpu); } }
    void Advance(const u32 steps, const f32 dt) { if (nested_) { nested_->Advance(steps, dt); } else { single_->Advance(steps, dt); } }
    [[nodiscard]] weather_gpu::StormCheckpoint Save() { return nested_ ? nested_->Save() : single_->Save(); }
    void Restore(const weather_gpu::StormCheckpoint& c) { if (nested_) { nested_->Restore(c); } else { single_->Restore(c); } }
    void ExportCloud(const weather_gpu::StormPlacement& p) { if (nested_) { nested_->ExportCloud(p); } else { single_->ExportCloud(p); } }
    [[nodiscard]] rhi::Buffer& CloudBuffer() { return nested_ ? nested_->CloudBuffer() : single_->CloudBuffer(); }
    [[nodiscard]] double Time() const { return nested_ ? nested_->Time() : single_->Time(); }
    [[nodiscard]] double LastAdvanceMilliseconds() const { return nested_ ? nested_->LastAdvanceMilliseconds() : single_->LastAdvanceMilliseconds(); }
    [[nodiscard]] std::size_t ResidentBytes() const { return nested_ ? nested_->ResidentBytes() : single_->ResidentBytes(); }
    // The grid the lightning is computed on: the nested child (500 m in all three directions) or the single grid; its state into a CPU solver of
    // that configuration (one GPU round trip), and where its west and south edges are from the domain's centre.
    [[nodiscard]] const weather_lab::FastStormConfig& LightningConfig() const { return nested_ ? nested_->Child().Config() : single_->Config(); }
    void DownloadLightning(weather_lab::FastStormSolver& cpu) { if (nested_) { nested_->Child().Download(cpu); } else { single_->Download(cpu); } }
    void LightningWestSouth(f64& east, f64& north) const
    {
        if (nested_)
        {
            float e = 0.0F, n = 0.0F;
            nested_->ChildWestSouthEdge(e, n);
            east = e;
            north = n;
        }
        else
        {
            east = -0.5 * single_->Config().nx * single_->Config().dx;
            north = -0.5 * single_->Config().ny * single_->Config().dy;
        }
    }
    [[nodiscard]] bool Nested() const { return nested_ != nullptr; }
    [[nodiscard]] f32 ChildSpacing() const { return childSpacing_; }

private:
    std::unique_ptr<weather_gpu::GpuStormSolver> single_;
    std::unique_ptr<weather_gpu::NestedGpuSolver> nested_;
    f32 childSpacing_{0.0F};
};

// Lightning from the simulated storm: the storm's own graupel, ice, supercooled water and updraft charge a window of its grid around the
// ice (storm_electrification, on a worker thread), and the flashes the breakdown produces come back in storm coordinates. The charge state
// lives on while the fields are refreshed from the solver, so the charge builds and discharges with the storm.
class StormLightningFeed
{
public:
    static constexpr u32 kWindowCells = 128U;      // horizontal cells of the electrified window at most (64 km at 500 m: the whole nested grid)
    static constexpr f64 kIntervalSeconds = 10.0;  // storm seconds gathered per job
    static constexpr f64 kMaxJobSeconds = 120.0;   // a longer backlog (a time jump) is dropped

    struct Flash
    {
        optical_transport::DischargeGraph graph{optical_transport::FlashKind::IntraCloud};
        f64 delaySeconds{0.0};   // storm seconds after the start of its job
        f64 jobSeconds{0.0};
    };

    ~StormLightningFeed() { Stop(); }

    void Reset()
    {
        Stop();
        started_ = false;
        pending_ = 0.0;
        lastTime_ = -1.0;
        std::lock_guard lock(mutex_);
        ready_.clear();
        blocks_.reset();
    }

    // Called every frame the storm runs normally; `stormTime` is the solver's time. Returns the flashes the worker finished since the last call.
    std::vector<Flash> Update(StormSim& sim, weather_lab::FastStormSolver& readout, const f64 stormTime, const StormLightningSettings& settings)
    {
        if (settings.chargeBoost != chargeBoost_ || settings.breakdownScale != breakdownScale_ || settings.flashSize != flashSize_ || settings.groundStrikeChance != groundStrikeChance_ || settings.smallFlashFraction != smallFlashFraction_)
        {
            // The source is built with these; a new value starts the charge over.
            Reset();
            chargeBoost_ = settings.chargeBoost;
            breakdownScale_ = settings.breakdownScale;
            flashSize_ = settings.flashSize;
            groundStrikeChance_ = settings.groundStrikeChance;
            smallFlashFraction_ = settings.smallFlashFraction;
        }
        if (lastTime_ < 0.0 || stormTime < lastTime_ || stormTime - lastTime_ > kMaxJobSeconds)
        {
            // First frame, or a jump: the charge belongs to a different storm moment.
            const bool jumped = lastTime_ >= 0.0;
            if (jumped) { Reset(); }
            lastTime_ = stormTime;
            return {};
        }
        pending_ = std::min(pending_ + (stormTime - lastTime_), kMaxJobSeconds);
        lastTime_ = stormTime;
        bool idle;
        {
            std::lock_guard lock(mutex_);
            idle = !busy_;
        }
        if (idle && pending_ >= kIntervalSeconds)
        {
            Submit(sim, readout);
        }
        std::lock_guard lock(mutex_);
        return std::exchange(ready_, {});
    }

    // Where the window sits: cell offsets of its corner in the parent grid (valid once a job was submitted).
    // The window's west and south edges, metres from the west and south edges of the grid it was cut from.
    [[nodiscard]] f64 OriginEast() const { return originEast_; }
    [[nodiscard]] f64 OriginNorth() const { return originNorth_; }
    [[nodiscard]] u32 Cells() const { return cells_; }
    [[nodiscard]] u64 FlashCount() const { return flashes_.load(); }
    [[nodiscard]] u64 GroundFlashCount() const { return groundFlashes_.load(); }
    [[nodiscard]] u32 NzLevels() const { return nz_; }
    [[nodiscard]] f64 JobMilliseconds() const
    {
        std::lock_guard lock(mutex_);
        return jobMilliseconds_;
    }
    [[nodiscard]] f32 SpacingMeters() const { return spacing_; }
    // The charge of the last finished job in blocks of 4 x 4 x 4 cells (null until one has finished).
    [[nodiscard]] std::shared_ptr<const StormChargeBlocks> Blocks() const
    {
        std::lock_guard lock(mutex_);
        return blocks_;
    }
    [[nodiscard]] bool Started() const { return started_; }

private:
    struct Job
    {
        storm_electrification::StormFields fields;
        f64 seconds{0.0};
        f64 chargeBoost{1.0};
        f64 breakdownScale{1.0};
        f64 flashSize{1.0};
        f64 groundStrikeChance{0.0};
        f64 smallFlashFraction{0.0};
        std::vector<f32> extinction;   // per metre, per cell of the window (see StormChargeBlocks)
    };

    void Submit(StormSim& sim, weather_lab::FastStormSolver& readout)
    {
        sim.DownloadLightning(readout);
        const weather_lab::FastStormConfig& c = readout.Config();
        std::vector<f32> th, qc, qi, qs, qg, w;
        readout.CopyField("th", th);
        readout.CopyField("qc", qc);
        readout.CopyField("qi", qi);
        readout.CopyField("qs", qs);
        readout.CopyField("qg", qg);
        readout.CopyField("w", w);
        const u32 nx = c.nx, ny = c.ny, nz = c.nz;
        const std::size_t plane = static_cast<std::size_t>(nx) * ny;
        // The charge grid has cubic cells: its spacing is the solver's vertical one, and the horizontal cells are sampled from the solver's
        // grid (the nested child is 500 m everywhere, so there it is exact; a coarse single grid is repeated over its wider cells).
        const f64 h = c.dz;
        const u32 window = static_cast<u32>(std::min<f64>(kWindowCells, std::floor(std::min(nx * c.dx, ny * c.dy) / h)));
        if (!started_)
        {
            // Centre the window on the column carrying the most ice and graupel.
            f64 sx = 0.0, sy = 0.0, total = 0.0;
            for (u32 k = 0; k < nz; ++k)
            {
                for (u32 j = 0; j < ny; ++j)
                {
                    for (u32 i = 0; i < nx; ++i)
                    {
                        const std::size_t cell = k * plane + static_cast<std::size_t>(j) * nx + i;
                        const f64 m = static_cast<f64>(qg[cell]) + qi[cell] + qs[cell];
                        sx += m * (i + 0.5);
                        sy += m * (j + 0.5);
                        total += m;
                    }
                }
            }
            const f64 cx = total > 0.0 ? sx / total * c.dx : 0.5 * nx * c.dx;
            const f64 cy = total > 0.0 ? sy / total * c.dy : 0.5 * ny * c.dy;
            originEast_ = std::clamp(cx - 0.5 * window * h, 0.0, std::max(0.0, nx * c.dx - window * h));
            originNorth_ = std::clamp(cy - 0.5 * window * h, 0.0, std::max(0.0, ny * c.dy - window * h));
            cells_ = window;
            nz_ = nz;
            spacing_ = static_cast<f32>(h);
            started_ = true;
        }
        Job job;
        const std::size_t count = static_cast<std::size_t>(window) * window * nz;
        storm_electrification::StormFields& f = job.fields;
        f.temperatureK.resize(count);
        f.airDensity.resize(count);
        f.updraft.resize(count);
        f.graupel.resize(count);
        f.ice.resize(count);
        f.liquid.resize(count);
        job.extinction.resize(count);
        const weather_lab::BaseState& base = readout.Base();
        std::size_t out = 0;
        for (u32 k = 0; k < nz; ++k)
        {
            for (u32 j = 0; j < window; ++j)
            {
                const u32 sj = std::min(ny - 1U, static_cast<u32>((originNorth_ + (j + 0.5) * h) / c.dy));
                for (u32 i = 0; i < window; ++i, ++out)
                {
                    const u32 si = std::min(nx - 1U, static_cast<u32>((originEast_ + (i + 0.5) * h) / c.dx));
                    const std::size_t cell = k * plane + static_cast<std::size_t>(sj) * nx + si;
                    f.temperatureK[out] = th[cell] * base.exner[k];
                    f.airDensity[out] = base.density[k];
                    f.updraft[out] = w[cell];
                    f.graupel[out] = qg[cell];
                    f.ice[out] = qi[cell] + qs[cell];
                    f.liquid[out] = qc[cell];
                    // Extinction of the particles, sigma = 3 W / (2 rho_w r_eff) for a water content W (kg/m3): droplets of 10 um, ice crystals
                    // of 40 um and 900 kg/m3, graupel of 0.5 mm and 400 kg/m3; the rain and the precipitation are in the renderer's own cloud.
                    const f32 density = base.density[k];
                    job.extinction[out] = density * (150.0F * qc[cell] + 41.0F * qi[cell] + 7.5F * qs[cell] + 7.5F * qg[cell]);
                }
            }
        }
        job.seconds = pending_;
        job.chargeBoost = chargeBoost_;
        job.breakdownScale = breakdownScale_;
        job.flashSize = flashSize_;
        job.groundStrikeChance = groundStrikeChance_;
        job.smallFlashFraction = smallFlashFraction_;
        pending_ = 0.0;
        {
            std::lock_guard lock(mutex_);
            job_ = std::move(job);
            hasJob_ = true;
            busy_ = true;
        }
        if (!worker_.joinable())
        {
            worker_ = std::thread([this] { Run(); });
        }
        wake_.notify_one();
    }

    void Run()
    {
        std::unique_ptr<storm_electrification::StormLightningSource> source;
        for (;;)
        {
            Job job;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [this] { return hasJob_ || stop_; });
                if (stop_) { return; }
                job = std::move(job_);
                hasJob_ = false;
            }
            if (!source)
            {
                storm_electrification::StormLightningParams params;
                params.timeScale = 1.0;
                params.maxFlashesPerStep = 12U;  // a full flash costs a potential solve over the window, a small one almost nothing
                params.electrification.chargingEfficiency *= job.chargeBoost;
                params.leader.initiationField *= job.breakdownScale;
                params.leader.propagationField *= job.breakdownScale;
                // A flash stops once it could neutralise this much charge. A boosted charge rate makes every cell hold that many times more, so the
                // budget scales with it: the boost changes how often the storm flashes, not how big a flash is; flash_size changes the size.
                params.leader.maxFlashCharge *= job.flashSize * job.chargeBoost;
                // Grow the channel as a tree with a trunk (see LeaderParams), not as a mesh with the same current in every branch.
                params.leader.growthExponent = 4.0;
                params.leader.sideBranchChance = 0.004;
                params.leader.directionPersistence = 6.0;
                params.leader.screening = 0.15;   // a conducting channel weakens the field at the cells that hug it: it crosses the cloud, it does not pack it
                params.leader.picksDivisor = 1.0e9;   // one new cell per round: a leader that grows from its strongest tip, branching only now and then
                params.leader.groundSeekChance = job.groundStrikeChance;
                params.leader.smallFlashFraction = job.smallFlashFraction;   // most flashes of a storm are small and cheap
                params.leader.goalBias = 6.0;                                // a leader that runs to where it is going, no loops
                params.leader.initiationSpread = 0.4;                        // not all flashes at the strongest cell
                params.leader.physicalEmission = true;   // the leader, return stroke, later strokes and cooling of LightningEmission.hpp
                params.leader.leaderSpeed = 2.5e6;   // the whole growth of a 400-cell channel takes some 50 ms: the arc is as quick as the light
                params.leader.currentWeighted = true;
                params.leader.diagonalGrowth = true;
                // A real flash has some 50 to 200 km of channel (100 to 400 cells of 500 m); the default cap of 6000 cells let one fill the volume.
                params.leader.maxChannelCells = 400U;
                source = std::make_unique<storm_electrification::StormLightningSource>(
                    std::move(job.fields), cells_, cells_, nz_, static_cast<f64>(spacing_), params);
            }
            else
            {
                source->UpdateFields(std::move(job.fields));
            }
            const auto jobStart = std::chrono::steady_clock::now();
            const f64 before = source->SimulatedSeconds();
            std::vector<storm_electrification::FlashResult> found = source->Advance(job.seconds);
            // The charge in blocks of 4 x 4 x 4 cells, for the grid display.
            auto blocks = std::make_shared<StormChargeBlocks>();
            {
                constexpr u32 kBlock = 4U;
                const storm_electrification::ChargeGrid& grid = source->Grid();
                blocks->nx = grid.Nx() / kBlock;
                blocks->ny = grid.Ny() / kBlock;
                blocks->nz = grid.Nz() / kBlock;
                blocks->blockMeters = static_cast<f32>(grid.Spacing() * kBlock);
                blocks->coulombs.assign(static_cast<std::size_t>(blocks->nx) * blocks->ny * blocks->nz, 0.0F);
                const f64 cellVolume = grid.Spacing() * grid.Spacing() * grid.Spacing();
                for (u32 k = 0; k < blocks->nz * kBlock; ++k)
                {
                    for (u32 j = 0; j < blocks->ny * kBlock; ++j)
                    {
                        for (u32 i = 0; i < blocks->nx * kBlock; ++i)
                        {
                            const std::size_t cell = grid.Index(i, j, k);
                            const f64 q = (grid.IceCharge()[cell] + grid.GraupelCharge()[cell]) * cellVolume;
                            blocks->coulombs[(static_cast<std::size_t>(k / kBlock) * blocks->ny + j / kBlock) * blocks->nx + i / kBlock] += static_cast<f32>(q);
                        }
                    }
                }
            }
            std::lock_guard lock(mutex_);
            jobMilliseconds_ = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - jobStart).count();
            blocks->extinctionCells = cells_;
            blocks->extinctionLevels = nz_;
            blocks->extinctionSpacing = spacing_;
            blocks->extinction = std::move(job.extinction);
            blocks_ = std::move(blocks);
            for (auto& flash : found)
            {
                Flash out;
                out.delaySeconds = std::clamp(flash.startTime - before, 0.0, job.seconds);
                out.jobSeconds = job.seconds;
                storm_electrification::RebaseFlashTimes(flash);
                out.graph = std::move(flash.graph);
                if (flash.kind == optical_transport::FlashKind::CloudToGround) { ++groundFlashes_; }
                ready_.push_back(std::move(out));
                ++flashes_;
            }
            busy_ = false;
        }
    }

    void Stop()
    {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        if (worker_.joinable()) { worker_.join(); }
        std::lock_guard lock(mutex_);
        stop_ = false;
        hasJob_ = false;
        busy_ = false;
    }

    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    Job job_;
    bool hasJob_{false};
    bool busy_{false};
    bool stop_{false};
    std::vector<Flash> ready_;
    std::shared_ptr<const StormChargeBlocks> blocks_;
    f64 jobMilliseconds_{0.0};
    std::atomic<u64> flashes_{0};
    std::atomic<u64> groundFlashes_{0};
    bool started_{false};
    f64 pending_{0.0};
    f64 lastTime_{-1.0};
    f64 chargeBoost_{1.0};
    f64 breakdownScale_{1.0};
    f64 flashSize_{4.0};
    f64 groundStrikeChance_{0.25};
    f64 smallFlashFraction_{0.75};
    f64 originEast_{0.0}, originNorth_{0.0};
    u32 cells_{0}, nz_{0};
    f32 spacing_{500.0F};
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
    celestial_clouds::StormZoneRuntime zoneRuntime{};
    celestial_clouds::WeatherZoneEvolutionRunner zoneEvolution{};
    u64 lastReconciledZoneGeneration{0};
    u64 zoneBodyGeneration{0};
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
    StormLightningFeed lightning;
    u32 lastFlashCells{0U};
    f64 lastFlashRadius{0.0};
    f64 maxFlashRadius{0.0};
    u64 readId{~0ULL};
    std::unique_ptr<weather_lab::FastStormSolver> cpuRead;   // the parent's state read back for the lightning

    struct Cached
    {
        f64 age;
        weather_gpu::StormCheckpoint data;
    };
    std::map<u64, std::vector<Cached>> checkpoints;
    std::map<u64, u64> lastUse;
    u64 useCounter{0};
    std::size_t checkpointBytes{0};
    std::string shaderSource;   // the last reloaded kernel source, applied to solvers built afterwards (empty: the embedded copy)
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

    if (const auto reload = stormMailbox_.TakeShaderReload(); reload.has_value())
    {
        // Hot iteration: the kernels are recompiled in place; a failure keeps the running pipelines (GpuStormSolver::ReloadShaders).
        std::string source;
        std::string name = "embedded";
        bool readable = true;
        if (!reload->empty())
        {
            std::ifstream file(*reload, std::ios::binary);
            std::ostringstream text;
            text << file.rdbuf();
            source = text.str();
            name = std::filesystem::path(*reload).filename().string();
            readable = file.good() || !source.empty();
        }
        if (!readable)
        {
            stormMailbox_.PublishShaderReload("failed: cannot read " + *reload, false);
        }
        else
        {
            std::string error;
            try
            {
                error = rt.solver ? rt.solver->ReloadShaders(source) : std::string();
            }
            catch (const std::exception& exception)
            {
                error = exception.what();
            }
            if (error.empty())
            {
                rt.shaderSource = source;
                rt.exported = false;   // draw the storm again with the new kernels even when the clock is paused
                stormMailbox_.PublishShaderReload("ok " + name, true);
            }
            else
            {
                stormMailbox_.PublishShaderReload("failed: " + error, false);
            }
        }
    }

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
    // Sparse opportunity discovery follows the authoritative planetary clock,
    // even when automatic storms are disabled. The worker never blocks rendering.
    if (!rt.bodySet || rt.body != body)
    {
        ++rt.zoneBodyGeneration;
        rt.zoneEvolution.Reset();
        rt.lastReconciledZoneGeneration = 0;
        rt.body = body;
        rt.bodySet = true;
    }
    celestial_clouds::WeatherParameters zoneWeather;
    zoneWeather.subsolarLatitudeRadians =
        std::asin(std::clamp(static_cast<f64>(sunDirectionBody.y), -1.0, 1.0));
    zoneWeather.noiseScale = layer.weatherScale;
    zoneWeather.detailScale = layer.detailScale;
    zoneWeather.seed = layer.seed;
    rt.zoneRuntime.Tick(rt.zoneBodyGeneration, nowSeconds, zoneWeather,
        celestial_clouds::WeatherClimate{});
    const auto zoneSnapshot = rt.zoneRuntime.Snapshot();
    rt.zoneEvolution.SetEvaluator(&celestial_clouds::EvaluateEmergentWeather);
    // Shared asynchronous worker: no per-zone thread, no GPU involvement.
    // Only validated detailed tickets can complete; long-step work stays blocked.
    rt.zoneEvolution.Tick(*zoneSnapshot, nowSeconds, planetRadiusMeters);
    const auto zoneScheduleStats=rt.zoneEvolution.Scheduler().Stats();

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
    status.discoveredZones = static_cast<u32>(zoneSnapshot->zones.size());
    status.zoneCandidateProbes = zoneSnapshot->candidates;
    status.zoneGeneration = zoneSnapshot->generation;
    status.zoneSnapshotSeconds = zoneSnapshot->simulatedSeconds;
    status.zoneSurveyBusy = rt.zoneRuntime.Busy();
    status.zoneWorkTickets = zoneScheduleStats.issued;
    status.zoneBlockedLongSteps = zoneScheduleStats.blockedLongSteps;
    status.zoneDeferredTasks = zoneScheduleStats.deferred;
    status.zoneMaximumLagSeconds = zoneScheduleStats.maximumLagSeconds;
    status.zoneCompletedSteps = rt.zoneEvolution.Completed();
    status.zonePendingExchanges = static_cast<u32>(
        rt.zoneEvolution.Ledger().PendingExchanges().size());
    status.zoneCrossZoneSources = static_cast<u32>(rt.zoneEvolution.CrossZoneSources());
    status.mode = mode;
    status.scheduledNearby = scheduledNearby;
    status.checkpointMiB = static_cast<f64>(rt.checkpointBytes) / (1024.0 * 1024.0);

    if (!wanted.valid || rt.body != body)
    {
        rt.active.valid = false;
        PublishStormCloudExport(nullptr);
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
            if (!rt.shaderSource.empty())
            {
                // A kernel edit reloaded earlier applies to this new solver too.
                if (const std::string error = rt.solver->ReloadShaders(rt.shaderSource); !error.empty())
                {
                    log::Warning("Storm kernels from the last reload do not compile on this grid: " + error);
                }
            }
            rt.solverGridKey = gridKey;
            rt.checkpoints.clear();
            rt.lastUse.clear();
            rt.checkpointBytes = 0;
            rt.haveState = false;
            rt.initId = ~0ULL;
        }
        if (switched)
        {
            rt.lightning.Reset();
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
            PublishStormCloudExport(&rt.solver->CloudBuffer());
        }
        else
        {
            PublishStormCloudExport(nullptr);
        }

        // ---- lightning from the storm's own charge (the flashes light the clouds when ORBIT_CLOUD_POINT_LIGHTS=1)
        const StormLightningSettings lightningSettings = stormMailbox_.Lightning();
        if (!stillBehind && intensity > 0.0 && lightningSettings.enabled)
        {
            if (!rt.cpuRead || rt.readId != wanted.id)
            {
                rt.cpuRead = std::make_unique<weather_lab::FastStormSolver>(rt.solver->LightningConfig());
                rt.readId = wanted.id;
                rt.lightning.Reset();
            }
            std::vector<StormLightningFeed::Flash> flashes = rt.lightning.Update(*rt.solver, *rt.cpuRead, rt.solver->Time(), lightningSettings);
            if (rt.lightning.Started())
            {
                // Where the electrified window is, for the grid display (see ElectrificationGridLines).
                const f64 cl = std::cos(latitude), sl = std::sin(latitude), co = std::cos(longitude), so = std::sin(longitude);
                f64 gridWest = 0.0, gridSouth = 0.0;
                rt.solver->LightningWestSouth(gridWest, gridSouth);
                lightningFrame_ = {true,
                    math::Double3{planetRadiusMeters * cl * co, planetRadiusMeters * sl, -planetRadiusMeters * cl * so},
                    math::Double3{-so, 0.0, -co}, math::Double3{-sl * co, cl, sl * so}, math::Double3{cl * co, sl, -cl * so},
                    gridWest + rt.lightning.OriginEast(), gridSouth + rt.lightning.OriginNorth(),
                    static_cast<f64>(rt.lightning.SpacingMeters()), rt.lightning.Cells(), rt.lightning.NzLevels()};
                lightningBlocks_ = rt.lightning.Blocks();
            }
            else
            {
                lightningFrame_.valid = false;
            }
            if (!flashes.empty())
            {
                const f64 cl = std::cos(latitude), sl = std::sin(latitude), co = std::cos(longitude), so = std::sin(longitude);
                const math::Double3 centre{planetRadiusMeters * cl * co, planetRadiusMeters * sl, -planetRadiusMeters * cl * so};
                const math::Double3 east{-so, 0.0, -co};
                const math::Double3 north{-sl * co, cl, sl * so};
                const math::Double3 up{cl * co, sl, -cl * so};
                const f64 stretch = lightningSettings.slowMotion;
                f64 gridWest = 0.0, gridSouth = 0.0;
                rt.solver->LightningWestSouth(gridWest, gridSouth);
                const f64 offsetE = gridWest + rt.lightning.OriginEast();
                const f64 offsetN = gridSouth + rt.lightning.OriginNorth();
                for (auto& flash : flashes)
                {
                    {
                        // Its size, for storm.status: the farthest point of the channel from the channel's centre.
                        math::Double3 sum{};
                        std::size_t points = 0;
                        for (const auto& segment : flash.graph.Segments())
                        {
                            sum = sum + math::Double3{segment.from[0], segment.from[1], segment.from[2]} + math::Double3{segment.to[0], segment.to[1], segment.to[2]};
                            points += 2U;
                        }
                        if (points > 0U)
                        {
                            const math::Double3 mean = sum * (1.0 / static_cast<f64>(points));
                            f64 radius = 0.0;
                            for (const auto& segment : flash.graph.Segments())
                            {
                                for (const double* p : {segment.from, segment.to})
                                {
                                    const math::Double3 d{p[0] - mean.x, p[1] - mean.y, p[2] - mean.z};
                                    radius = std::max(radius, std::sqrt(math::Dot(d, d)));
                                }
                            }
                            rt.lastFlashRadius = radius;
                            rt.lastFlashCells = static_cast<u32>(flash.graph.Segments().size());
                            rt.maxFlashRadius = std::max(rt.maxFlashRadius, radius);
                        }
                    }
                    f64 end = 0.0;
                    for (std::size_t i = 0; i < flash.graph.Segments().size(); ++i)
                    {
                        optical_transport::Segment& segment = flash.graph.At(static_cast<std::int32_t>(i));
                        // The pulse times are on the electrification clock; the registry wants them relative to the flash.
                        segment.formedAt *= stretch;
                        for (auto& pulse : segment.pulses)
                        {
                            pulse.startTime *= stretch;
                            pulse.duration *= stretch;
                            pulse.radiantEnergy *= stretch * lightningSettings.brightness;
                            end = std::max(end, pulse.startTime + pulse.duration);
                        }
                        for (double* p : {segment.from, segment.to})
                        {
                            const math::Double3 world = east * (offsetE + p[0]) + north * (offsetN + p[1]) + up * p[2];
                            p[0] = world.x;
                            p[1] = world.y;
                            p[2] = world.z;
                        }
                    }
                    // Spread the flashes of one job over the time it covers, so they do not arrive in a burst.
                    const std::scoped_lock flashLock(transientFlashMutex_);
                    transientFlashes_.Add(std::move(flash.graph), centre, FlashClockSeconds() + std::min(flash.delaySeconds / std::max(1.0, simulationRate_.load()), 10.0), end + 1.0);
                }
            }
        }
        else
        {
            lightningFrame_.valid = false;
            if (stillBehind) { rt.lightning.Reset(); }
        }
        status.lightningFlashes = rt.lightning.FlashCount();
        status.lightningGroundFlashes = rt.lightning.GroundFlashCount();
        status.lightningArcSegments = lastArcSegments_.load();
        status.lightningJobMilliseconds = rt.lightning.JobMilliseconds();
        status.lightningFlashCells = rt.lastFlashCells;
        status.lightningFlashRadiusMeters = rt.lastFlashRadius;
        status.lightningFlashRadiusMaxMeters = rt.maxFlashRadius;

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
        PublishStormCloudExport(nullptr);
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

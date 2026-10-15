#include <orbit/studio_ui/StormLightningController.hpp>

#include <orbit/storm_electrification/StormLightningSource.hpp>
#include <orbit/storm_electrification/WarmRainAdapter.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace orbit::studio_ui
{
namespace
{
// One tick does at most this much work, so Stop() returns promptly and a slow machine runs the storm slower than
// requested rather than ever further behind.
constexpr std::uint32_t kMaxStepsPerTick = 2U;

[[nodiscard]] storm_electrification::StormFields FieldsFromSnapshot(const weather_lab::StormSnapshot& snap)
{
    storm_electrification::WarmRainInputs in;
    in.potentialTemperatureK = snap.theta;
    in.cloudWater = snap.qc;
    in.rainWater = snap.qr;
    in.updraft = snap.w;
    const std::size_t plane = static_cast<std::size_t>(snap.nx) * snap.ny;
    in.exner.resize(snap.theta.size());
    in.airDensity.resize(snap.theta.size());
    for (std::size_t i = 0; i < snap.theta.size(); ++i)
    {
        in.exner[i] = snap.levelExner[i / plane];
        in.airDensity[i] = snap.levelDensity[i / plane];
    }
    return storm_electrification::StormFieldsFromWarmRain(in);
}
} // namespace

StormLightningController::StormLightningController(weather_lab::WeatherLabSession& session, Sink sink)
    : session_(session), sink_(std::move(sink))
{
}

StormLightningController::~StormLightningController()
{
    Stop();
}

std::string StormLightningController::Start(const StormLightningRequest& request)
{
    Stop();
    weather_lab::StormSnapshot snap = session_.LiveSnapshot();
    if (!snap.valid)
    {
        return "the Weather Lab has no live sample yet (weather_lab.control start or step first)";
    }
    // The charge grid has square cells, so the solver's horizontal and vertical spacing must agree.
    if (snap.dx <= 0.0F || std::abs(snap.dx - snap.dz) > 1.0e-3F * snap.dx)
    {
        return "lightning needs square cells: set the Weather Lab dx equal to dz";
    }
    if (snap.theta.size() != static_cast<std::size_t>(snap.nx) * snap.ny * snap.nz || snap.levelExner.size() < snap.nz ||
        snap.levelDensity.size() < snap.nz)
    {
        return "the Weather Lab snapshot is inconsistent";
    }
    storm_electrification::StormLightningParams params;
    params.timeScale = request.timeScale;
    auto source = std::make_shared<storm_electrification::StormLightningSource>(
        FieldsFromSnapshot(snap), snap.nx, snap.ny, snap.nz, static_cast<f64>(snap.dx), params);
    lighting::StormAnchor anchor;
    anchor.distanceMeters = request.distanceMeters;
    anchor.lateralMeters = request.lateralMeters;
    anchor.domainMeters = source->DomainSizeMeters();
    if (sink_.setAnchor)
    {
        sink_.setAnchor(anchor);
    }
    {
        std::lock_guard lock(statusMutex_);
        status_ = {true, 0.0, 0U, 0.0, anchor.domainMeters};
    }
    stop_ = false;
    worker_ = std::thread([this, source] {
        using Clock = std::chrono::steady_clock;
        auto last = Clock::now();
        while (!stop_)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const auto now = Clock::now();
            const f64 dt = std::chrono::duration<f64>(now - last).count();
            last = now;
            for (auto& flash : source->Advance(std::min(dt, 0.25), kMaxStepsPerTick))
            {
                if (sink_.addFlash)
                {
                    storm_electrification::RebaseFlashTimes(flash);
                    sink_.addFlash(std::move(flash.graph));
                }
            }
            std::lock_guard lock(statusMutex_);
            status_.simulatedSeconds = source->SimulatedSeconds();
            status_.flashes = source->FlashCount();
            status_.breakdownRatio = source->MaxBreakdownRatio();
        }
    });
    return {};
}

void StormLightningController::Stop()
{
    stop_ = true;
    if (worker_.joinable())
    {
        worker_.join();
    }
    std::lock_guard lock(statusMutex_);
    status_.running = false;
}

StormLightningStatus StormLightningController::Status() const
{
    std::lock_guard lock(statusMutex_);
    return status_;
}
} // namespace orbit::studio_ui

#pragma once

#include <orbit/lighting/TransientFlashRegistry.hpp>
#include <orbit/optical_transport/DischargeGraph.hpp>
#include <orbit/weather_lab/WeatherLabSession.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace orbit::studio_ui
{
struct StormLightningRequest
{
    f64 distanceMeters{12000.0};
    f64 lateralMeters{0.0};
    f64 timeScale{20.0};   // simulated storm seconds per wall second
};

struct StormLightningStatus
{
    bool running{false};
    f64 simulatedSeconds{0.0};
    u64 flashes{0U};
    f64 breakdownRatio{0.0};
    f64 domainMeters{0.0};
};

// Turns the Weather Lab's live storm into lightning: takes a snapshot of its fields, derives the ice phase with the
// temperature proxy, runs the electrification on a worker thread at `timeScale`, and gives every flash to the viewport
// that places it against a fixed anchor (see lighting::StormAnchor). The snapshot is frozen: Stop and Start again to
// pick up a newer storm.
class StormLightningController
{
public:
    struct Sink
    {
        std::function<void(const lighting::StormAnchor&)> setAnchor;
        std::function<void(optical_transport::DischargeGraph)> addFlash;
    };

    StormLightningController(weather_lab::WeatherLabSession& session, Sink sink);
    ~StormLightningController();
    StormLightningController(const StormLightningController&) = delete;
    StormLightningController& operator=(const StormLightningController&) = delete;

    // Empty string = started. Fails (and changes nothing) when the Weather Lab has no live sample yet.
    [[nodiscard]] std::string Start(const StormLightningRequest& request);
    void Stop();
    [[nodiscard]] StormLightningStatus Status() const;

private:
    weather_lab::WeatherLabSession& session_;
    Sink sink_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    mutable std::mutex statusMutex_;
    StormLightningStatus status_;
};
} // namespace orbit::studio_ui

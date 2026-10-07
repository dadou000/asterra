#pragma once

#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/rpc/JsonRpc.hpp>
#include <orbit/studio_session/SimulationClock.hpp>
#include <orbit/studio_session/StudioSession.hpp>

#include <functional>
#include <string>

namespace orbit::studio_ui
{
// The headless operations behind the simulation transport: pause / simulate /
// advance for the Studio clock that drives planetary rotation, orbits, the sun
// and the atmosphere and weather. The transport band and the time.* RPC / MCP
// methods all go through this one object, so a button and an agent do exactly
// the same thing.
class SimulationControls
{
public:
    explicit SimulationControls(studio_session::SimulationClock& clock) noexcept;

    [[nodiscard]] bool Playing() const noexcept;
    void SetPlaying(bool playing);
    void TogglePlaying();

    [[nodiscard]] f64 Rate() const noexcept;
    // Simulation seconds per real second. Negative runs time backwards.
    void SetRate(f64 simulationSecondsPerRealSecond);

    [[nodiscard]] i64 TimeMicroseconds() const noexcept;
    void SetTimeMicroseconds(i64 microseconds);

    // Advances (or, when negative, rewinds) by simulation seconds, whether
    // the clock is playing or paused.
    void Step(f64 simulationSeconds);

    // The size the Step buttons use.
    [[nodiscard]] f64 StepSeconds() const noexcept;
    void SetStepSeconds(f64 simulationSeconds);

    // "T+3d 04:05:06.250": time since the clock epoch.
    [[nodiscard]] static std::string FormatTime(i64 microseconds);

private:
    studio_session::SimulationClock* clock_{nullptr};
    f64 stepSeconds_{60.0};
};

// The transport band along the bottom of Studio: Simulate / Pause, step back
// and forward, step size, speed and the simulation time. An optional extra
// button raises an issue report (see ReportsUi).
class SimulationControlsUi
{
public:
    explicit SimulationControlsUi(
        SimulationControls& controls,
        studio_session::StudioSession& session) noexcept;
    ~SimulationControlsUi();

    SimulationControlsUi(const SimulationControlsUi&) = delete;
    SimulationControlsUi& operator=(const SimulationControlsUi&) = delete;

    void Register(editor_ui::EditorUi& ui);

    // Adds a "Report issue" button to the band.
    void SetReportIssueHandler(std::function<void()> handler);

    inline static constexpr const char* kBandId = "orbit.simulation";

private:
    void Draw(editor_ui::PanelContext& context);

    SimulationControls* controls_{nullptr};
    studio_session::StudioSession* session_{nullptr};
    std::function<void()> reportIssue_;
    bool registered_{false};
    i32 stepIndex_{2};
    i32 rateIndex_{1};
    f64 customRate_{1.0};
};

// time.get / time.set / time.step: the RPC face of the transport.
void RegisterSimulationRpc(
    rpc::Dispatcher& dispatcher,
    SimulationControls& controls,
    studio_session::StudioSession& session);
} // namespace orbit::studio_ui

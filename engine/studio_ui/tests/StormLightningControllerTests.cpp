// End to end on the CPU: the Weather Lab's real fast-core storm -> snapshot -> ice proxy -> electrification on a worker
// thread -> flashes handed to a sink.
#include <orbit/studio_ui/StormLightningController.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace orbit;
using namespace orbit::studio_ui;
using Clock = std::chrono::steady_clock;

namespace
{
int failures = 0;
void Check(bool ok, const char* what)
{
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}
bool WaitFor(const std::function<bool()>& done, double seconds)
{
    const auto end = Clock::now() + std::chrono::duration<double>(seconds);
    while (Clock::now() < end)
    {
        if (done()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return done();
}
} // namespace

int main()
{
    weather_lab::WeatherLabSession session;
    std::atomic<int> anchors{0}, flashes{0};
    std::atomic<double> domain{0.0};
    StormLightningController controller(session, {
        [&](const lighting::StormAnchor& a) { ++anchors; domain = a.domainMeters; },
        [&](optical_transport::DischargeGraph g) { if (!g.Segments().empty()) { ++flashes; } }});

    Check(!controller.Start({}).empty() && anchors == 0, "no live sample: start is refused and nothing is placed");

    weather_lab::WeatherLabSettings settings;
    settings.solver.nx = 64; settings.solver.ny = 64; settings.solver.nz = 40;
    settings.solver.dx = 2000.0F; settings.solver.dy = 2000.0F; settings.solver.dz = 500.0F;   // not square
    settings.frameIntervalSeconds = 300.0;
    Check(session.Configure(settings).empty(), "configure (non-square)");
    Check(session.Step(300.0).empty(), "step");
    Check(WaitFor([&] { return session.Status().state == weather_lab::SessionState::Paused && session.Status().simTime >= 299.0; }, 300.0), "stepped");
    const std::string squareError = controller.Start({});
    std::printf("non-square start: %s\n", squareError.c_str());
    Check(squareError.find("square") != std::string::npos, "non-square cells are refused with an explanation");
    session.Reset();

    settings.solver.dx = 500.0F; settings.solver.dy = 500.0F;
    settings.solver.bubble.horizontalRadius = 5000.0F;
    Check(session.Configure(settings).empty(), "configure (square)");
    Check(session.Step(1200.0).empty(), "step to a mature storm");
    Check(WaitFor([&] { return session.Status().state == weather_lab::SessionState::Paused && session.Status().simTime >= 1199.0; }, 600.0), "storm reached 20 minutes");

    StormLightningRequest request;
    request.timeScale = 200.0;
    const std::string error = controller.Start(request);
    Check(error.empty(), "start on a mature storm");
    Check(anchors == 1 && std::abs(domain - 32000.0) < 1.0, "anchor set once with the 32 km domain");
    const bool flashed = WaitFor([&] { return flashes > 0; }, 90.0);
    const StormLightningStatus status = controller.Status();
    std::printf("running %d, simulated %.0f s, flashes %llu (sink %d), ratio %.2f\n", status.running, status.simulatedSeconds,
                static_cast<unsigned long long>(status.flashes), flashes.load(), status.breakdownRatio);
    Check(flashed, "the mature storm produces flashes that reach the sink");
    Check(status.running && status.simulatedSeconds > 0.0, "status reports a running source");
    controller.Stop();
    Check(!controller.Status().running, "stop");
    std::printf(failures == 0 ? "StormLightningController tests passed\n" : "%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

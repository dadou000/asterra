#include <orbit/studio_ui/WeatherLabRpc.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <string>
#include <thread>

namespace
{
using orbit::rpc::Value;

void Check(
    const bool condition,
    const std::source_location location = std::source_location::current())
{
    if (!condition)
    {
        std::cerr << "Weather lab RPC test failed at " << location.file_name()
                  << ':' << location.line() << '\n';
        std::exit(1);
    }
}

struct Reply
{
    Value root;
    [[nodiscard]] bool Ok() const { return root.Find("result") != nullptr; }
    [[nodiscard]] const Value& Result() const { return *root.Find("result"); }
    [[nodiscard]] std::string Error() const
    {
        const Value* error = root.Find("error");
        return error == nullptr ? std::string() : error->Find("message")->AsString();
    }
};

Reply Call(
    const orbit::rpc::Dispatcher& dispatcher,
    const std::string& method,
    const std::string& params = "{}")
{
    const auto response = dispatcher.Dispatch(
        R"({"jsonrpc":"2.0","id":1,"method":")" + method
        + R"(","params":)" + params + "}");
    Check(response.has_value());
    return {orbit::rpc::ParseValue(*response)};
}

bool WaitForState(
    const orbit::rpc::Dispatcher& dispatcher, const std::string& state)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (std::chrono::steady_clock::now() < deadline)
    {
        const Reply status = Call(dispatcher, "weather_lab.status");
        if (status.Ok() && status.Result().Find("state")->AsString() == state)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

const char* kSmall =
    R"({"nx":16,"ny":16,"nz":40,"dx":4000,"dz":500,"max_time_step":24,)"
    R"("max_courant":2.5,"target_minutes":10,"frame_interval_s":300,"threads":2})";
} // namespace

int main()
{
    using namespace orbit;
    rpc::Dispatcher dispatcher;
    weather_lab::WeatherLabSession session;
    studio_ui::WeatherLabView view;

    // A fake Volume host: only the id "11111111-1111-1111-1111-111111111111" is a Volume.
    const std::string volumeId = "11111111-1111-1111-1111-111111111111";
    int attached = 0;
    int detached = 0;
    weather_lab::CloudVolumeGrid lastGrid;
    const studio_ui::WeatherLabVolumeSink sink =
        [&](const std::string& id, const weather_lab::CloudVolumeGrid* grid)
    {
        studio_ui::WeatherLabVolumeResult result;
        if (id != volumeId)
        {
            result.error = "object " + id + " is not a Volume";
            return result;
        }
        result.representationMode = "Auto";
        result.representationOk = false;
        result.currentHalfExtents[0] = 10.0;
        if (grid == nullptr)
        {
            ++detached;
            return result;
        }
        ++attached;
        lastGrid = *grid;
        result.recommendedHalfExtents[0] = grid->sizeX * 0.5;
        result.recommendedHalfExtents[1] = grid->sizeY * 0.5;
        result.recommendedHalfExtents[2] = grid->sizeZ * 0.5;
        return result;
    };
    studio_ui::RegisterWeatherLabRpc(dispatcher, session, view, sink);

    // Every method is registered; read-only ones are not flagged mutating.
    std::size_t registered = 0;
    for (const auto& method : dispatcher.Catalog())
    {
        if (method.name.rfind("weather_lab.", 0) == 0)
        {
            ++registered;
            const bool readOnly = method.name == "weather_lab.status"
                || method.name == "weather_lab.metrics"
                || method.name == "weather_lab.compare"
                || method.name == "weather_lab.slice";
            Check(method.mutating != readOnly);
        }
    }
    Check(registered == 9U);

    Reply status = Call(dispatcher, "weather_lab.status");
    Check(status.Ok());
    Check(status.Result().Find("state")->AsString() == "idle");

    // Validation errors carry messages and change nothing.
    Check(Call(dispatcher, "weather_lab.control", R"({"action":"explode"})").Error().find("action") != std::string::npos);
    Check(!Call(dispatcher, "weather_lab.control", R"({"action":"step"})").Ok());
    Check(!Call(dispatcher, "weather_lab.configure", R"({"nx":"many"})").Ok());
    Check(!Call(dispatcher, "weather_lab.configure", R"({"nx":48})").Ok()); // not a power of two
    Check(!Call(dispatcher, "weather_lab.slice", R"({"field":"bogus"})").Ok());
    Check(!Call(dispatcher, "weather_lab.slice").Ok()); // no live run yet

    Reply configured = Call(dispatcher, "weather_lab.configure", kSmall);
    Check(configured.Ok());
    Check(configured.Result().Find("nx")->AsInteger() == 16);
    Check(configured.Result().Find("advection")->AsString() == "cubic");

    // Step 300 s, then it pauses by itself.
    Check(Call(dispatcher, "weather_lab.control", R"({"action":"step","seconds":300})").Ok());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (std::chrono::steady_clock::now() < deadline
        && Call(dispatcher, "weather_lab.status").Result().Find("sim_time_s")->AsNumber() < 299.9)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    status = Call(dispatcher, "weather_lab.status");
    Check(status.Result().Find("state")->AsString() == "paused");
    Check(std::fabs(status.Result().Find("sim_time_s")->AsNumber() - 300.0) < 0.5);
    Check(status.Result().Find("diagnostics")->Find("max_divergence_per_s")->AsNumber() < 1.0e-6);
    // Settings are frozen while a run exists.
    Check(!Call(dispatcher, "weather_lab.configure", R"({"dz":250})").Ok());

    // Slices: summary by default, values on request, decimated on demand.
    Reply slice = Call(dispatcher, "weather_lab.slice", R"({"kind":"plan","field":"thp","height_m":250})");
    Check(slice.Ok() && slice.Result().Find("values") == nullptr);
    Check(slice.Result().Find("width")->AsInteger() == 16);
    slice = Call(dispatcher, "weather_lab.slice",
        R"({"kind":"section","field":"qv","include_values":true,"max_cells":8})");
    Check(slice.Ok());
    Check(slice.Result().Find("values_width")->AsInteger() == 8);
    Check(slice.Result().Find("values")->AsArray().size()
        == static_cast<std::size_t>(slice.Result().Find("values_width")->AsInteger()
            * slice.Result().Find("values_height")->AsInteger()));

    // Run on to the target and read metrics.
    Check(Call(dispatcher, "weather_lab.control", R"({"action":"start"})").Ok());
    Check(WaitForState(dispatcher, "finished"));
    Reply metrics = Call(dispatcher, "weather_lab.metrics");
    Check(metrics.Ok() && metrics.Result().Find("rows")->AsArray().size() == 3U);
    Check(!Call(dispatcher, "weather_lab.control", R"({"action":"start"})").Ok()); // needs reset

    // Playback needs a file; a missing one is a typed error.
    Check(!Call(dispatcher, "weather_lab.playback", R"({"action":"load","path":"/no/such.orbitwx"})").Ok());
    Check(!Call(dispatcher, "weather_lab.playback", R"({"action":"select","frame":0})").Ok());
    Check(Call(dispatcher, "weather_lab.compare").Result().Find("rows")->AsArray().empty());

    // The view round-trips and rejects unknown fields.
    Reply viewReply = Call(dispatcher, "weather_lab.view",
        R"({"plan_field":"qr","plan_height_m":1500,"section_row":3,"show_comparison":false})");
    Check(viewReply.Ok());
    Check(view.planField == "qr" && view.sectionRow == 3 && !view.showComparison);
    Check(view.planHeightMeters == 1500.0F);
    Check(!Call(dispatcher, "weather_lab.view", R"({"column_field":"nope"})").Ok());
    Check(view.columnField == "condensate");

    // Storm -> Volume: needs an id that is a Volume, a storm to convert, and
    // reports what the volume still needs.
    Check(!Call(dispatcher, "weather_lab.volume", R"({"action":"push"})").Ok());
    Check(!Call(dispatcher, "weather_lab.volume", R"({"action":"push","volume_id":"not-a-volume"})").Ok());
    Check(!Call(dispatcher, "weather_lab.volume", R"({"action":"push","volume_id":")" + volumeId + R"(","gain":0})").Ok());
    Reply pushed = Call(dispatcher, "weather_lab.volume",
        R"({"action":"push","volume_id":")" + volumeId + R"(","up_axis":"y","gain":0.8})");
    Check(pushed.Ok());
    Check(attached == 1 && lastGrid.valid && lastGrid.resolutionY == 40U);   // y is up: cache y = layers
    Check(pushed.Result().Find("representation_ok")->AsBool() == false);
    Check(pushed.Result().Find("recommended_half_extents_m")->Find("x")->AsNumber() == 32000.0); // 16 cells x 4 km / 2
    Check(pushed.Result().Find("resolution")->AsArray().size() == 3U);
    Check(view.volumeId == volumeId && view.volumeUpAxis == weather_lab::VolumeUpAxis::Y);
    Check(Call(dispatcher, "weather_lab.volume", R"({"action":"clear"})").Ok() && detached == 1);
    // The same operation is what the panel's button and auto-update call.
    view.volumeAuto = true;
    Check(studio_ui::PushStormVolume(session, view, sink).empty() && attached == 2);
    Check(!studio_ui::PushStormVolume(session, view, {}).empty());
    Check(Call(dispatcher, "weather_lab.view", R"({"volume_gain":2,"volume_up_axis":"z"})").Ok());
    Check(view.volumeGain == 2.0F && view.volumeUpAxis == weather_lab::VolumeUpAxis::Z);
    Check(!Call(dispatcher, "weather_lab.view", R"({"volume_up_axis":"w"})").Ok());

    Check(Call(dispatcher, "weather_lab.control", R"({"action":"reset"})").Ok());
    Check(Call(dispatcher, "weather_lab.status").Result().Find("state")->AsString() == "idle");
    Check(!Call(dispatcher, "weather_lab.volume", R"({"action":"push","volume_id":")" + volumeId + R"("})").Ok()); // no storm

    std::cout << "Weather lab RPC tests passed\n";
    return 0;
}

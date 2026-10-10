#pragma once

#include <orbit/rpc/JsonRpc.hpp>
#include <orbit/weather_lab/WeatherLabSession.hpp>

#include <functional>
#include <string>

namespace orbit::studio_ui
{
// What the Weather Lab panel is currently showing. The panel and the
// weather_lab.view RPC edit this one object; slices themselves can always be
// requested directly with weather_lab.slice, so nothing the panel can show is
// unreachable over RPC / MCP.
struct WeatherLabView
{
    weather_lab::DisplaySource source{weather_lab::DisplaySource::Live};
    std::string planField{"w"};
    f32 planHeightMeters{3000.0F};
    std::string sectionField{"w"};
    // -1 cuts through the strongest updraft.
    i32 sectionRow{-1};
    std::string columnField{"condensate"};
    bool showMetrics{true};
    bool showComparison{true};

    // Storm -> Volume object (baked-cache path). volumeId is the text id of a
    // Volume object; with volumeAuto the panel pushes a fresh density grid
    // whenever the displayed storm time changes.
    std::string volumeId;
    bool volumeAuto{false};
    f32 volumeGain{0.5F};
    weather_lab::VolumeUpAxis volumeUpAxis{weather_lab::VolumeUpAxis::Y};
};

// What attaching a density grid to a Volume object found. Filled by the Studio
// bridge (WeatherLabVolumeBridge); faked in tests.
struct WeatherLabVolumeResult
{
    std::string error;
    // The volume's representation mode must be "Baked" for the cache to draw.
    std::string representationMode;
    bool representationOk{false};
    // The volume's current half extents and those that would show the whole
    // storm one-to-one, along the volume's own x/y/z axes, metres.
    f64 currentHalfExtents[3]{};
    f64 recommendedHalfExtents[3]{};
};

// Attaches `grid` to the Volume object named by the text id; empty `grid`
// means detach. Returns an error in the result when the id is not a Volume.
using WeatherLabVolumeSink = std::function<WeatherLabVolumeResult(
    const std::string& volumeId, const weather_lab::CloudVolumeGrid* grid)>;

// The one operation behind the panel's Push to Volume button, the auto-update
// and weather_lab.volume: converts the displayed storm and hands it to the
// sink. Empty string = pushed.
[[nodiscard]] std::string PushStormVolume(
    weather_lab::WeatherLabSession& session,
    const WeatherLabView& view,
    const WeatherLabVolumeSink& sink,
    WeatherLabVolumeResult* result = nullptr,
    weather_lab::CloudVolumeGrid* pushed = nullptr);

// weather_lab.status / configure / control / playback / metrics / compare /
// slice / view / volume: the RPC face of the Weather Lab panel and of the session behind
// it (docs/ORBIT_MCP.md). Session and view must outlive the dispatcher.
void RegisterWeatherLabRpc(
    rpc::Dispatcher& dispatcher,
    weather_lab::WeatherLabSession& session,
    WeatherLabView& view,
    WeatherLabVolumeSink volumeSink = {});
} // namespace orbit::studio_ui

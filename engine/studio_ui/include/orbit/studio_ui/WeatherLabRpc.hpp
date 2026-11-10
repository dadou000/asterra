#pragma once

#include <orbit/rpc/JsonRpc.hpp>
#include <orbit/weather_lab/WeatherLabSession.hpp>

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
};

// weather_lab.status / configure / control / playback / metrics / compare /
// slice / view: the RPC face of the Weather Lab panel and of the session behind
// it (docs/ORBIT_MCP.md). Session and view must outlive the dispatcher.
void RegisterWeatherLabRpc(
    rpc::Dispatcher& dispatcher,
    weather_lab::WeatherLabSession& session,
    WeatherLabView& view);
} // namespace orbit::studio_ui

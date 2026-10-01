#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/editor_ui/EditorUi.hpp>

#include <array>
#include <chrono>
#include <map>
#include <string>
#include <string_view>

namespace orbit::studio_ui
{
class StudioRenderViewSet;

// Draws the text diagnostics readout over a viewport image when the view has
// it switched on (StudioRenderViewSet::TextDiagnosticsHud). The report samples
// the terrain source about a hundred times, so each view's text is rebuilt only
// when the camera or cursor moved or it is older than a short interval.
class StudioTextDiagnosticsHud
{
public:
    // Call immediately after submitting the viewport Image: the readout is
    // anchored to that item. A no-op when the HUD is off for `id`.
    void Draw(
        editor_ui::PanelContext& context,
        StudioRenderViewSet& views,
        std::string_view id,
        const editor_ui::ImageInteraction& interaction);

private:
    struct Cache
    {
        std::string text;
        std::array<f64, 11> key{};
        std::chrono::steady_clock::time_point builtAt{};
        bool valid{false};
    };

    std::map<std::string, Cache, std::less<>> caches_;
};
} // namespace orbit::studio_ui

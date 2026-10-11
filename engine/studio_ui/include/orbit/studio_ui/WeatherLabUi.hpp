#pragma once

#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/studio_ui/StormDomain.hpp>
#include <orbit/studio_ui/WeatherLabRpc.hpp>
#include <orbit/weather_lab/WeatherLabSession.hpp>

#include <string>

namespace orbit::studio_ui
{
// The Weather Lab panel (SC-01): run the compressed fast-core supercell in
// Studio, watch plan / section / column-max slices of the storm, and compare
// its metrics with a CM1 reference loaded from an .orbitwx file. The panel
// owns no simulation logic: every button calls WeatherLabSession, the same
// object the weather_lab.* RPC / MCP methods (RegisterWeatherLabRpc) drive.
class WeatherLabUi
{
public:
    WeatherLabUi(weather_lab::WeatherLabSession& session, WeatherLabView& view);

    void Register(editor_ui::EditorUi& ui);
    // The mailbox of the viewport renderer's storm domain (the same object the storm.* RPC drives); without it the section is hidden.
    void SetStormDomain(StormDomainMailbox* mailbox) noexcept { storm_ = mailbox; }

    inline static constexpr editor_ui::PanelId kPanelId{
        .high = 0x5745415448455231ULL,
        .low = 0x4c41425343303100ULL
    };

private:
    void Draw(editor_ui::PanelContext& context);
    void DrawControls(editor_ui::PanelContext& context);
    void DrawSettings(editor_ui::PanelContext& context);
    void DrawPlayback(editor_ui::PanelContext& context);
    void DrawStorm(editor_ui::PanelContext& context);
    void DrawViews(editor_ui::PanelContext& context);
    void DrawMetrics(editor_ui::PanelContext& context);
    void DrawSlice(
        editor_ui::PanelContext& context,
        std::string_view id,
        const weather_lab::Slice& slice,
        editor_ui::UiSize size);

    weather_lab::WeatherLabSession* session_{nullptr};
    WeatherLabView* view_{nullptr};
    StormDomainMailbox* storm_{nullptr};
    StormPlaceRequest stormDraft_;
    StormAutoRequest stormAuto_;
    bool stormAutoLoaded_{false};
    std::string stormMessage_;
    weather_lab::WeatherLabSettings draft_;
    bool draftLoaded_{false};
    std::string playbackPath_;
    std::string recordPathText_;
    std::string message_;
    i32 presetIndex_{0};
};
} // namespace orbit::studio_ui

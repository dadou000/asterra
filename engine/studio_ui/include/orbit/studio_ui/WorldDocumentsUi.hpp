#pragma once

#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/studio_session/StudioSession.hpp>

#include <filesystem>
#include <optional>
#include <string>

namespace orbit::studio_ui
{
// Session-bound world document management for an already-open project.
// This panel never owns or reopens project authority: every operation is
// routed through the exact StudioSession used by Explorer/runtime/RPC.
class WorldDocumentsUi
{
public:
    explicit WorldDocumentsUi(
        studio_session::StudioSession& session,
        bool allowCloseWorld = true) noexcept;

    void Register(editor_ui::EditorUi& ui);

    inline static constexpr editor_ui::PanelId kPanelId{
        .high = 0x4f52424954535455ULL,
        .low = 0x574f524c44444f43ULL
    };

private:
    void Draw(editor_ui::PanelContext& context);

    studio_session::StudioSession* session_{nullptr};
    bool allowCloseWorld_{true};
    std::string createWorldPath_{"Worlds/NewWorld.orbitworld"};
    std::string createWorldName_{"New World"};
    std::optional<std::filesystem::path> selectedWorld_;
    std::string selectedWorldName_;
    std::string status_;
};
} // namespace orbit::studio_ui

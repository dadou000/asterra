#pragma once

#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/volume_fields/VolumeFieldStorage.hpp>

#include <string>

namespace orbit::studio_ui
{
class VolumeAuthoringUi
{
public:
    VolumeAuthoringUi(
        studio_session::StudioSession& session,
        volume_fields::VolumeFieldStorageService& fields) noexcept;

    void Register(
        editor_ui::EditorUi& ui);

    inline static constexpr editor_ui::PanelId kPanel{
        .high = 0x4f52424954535455ULL,
        .low = 0x564f4c554d455330ULL
    };

private:
    void Draw(
        editor_ui::PanelContext& context);

    studio_session::StudioSession* session_{nullptr};
    volume_fields::VolumeFieldStorageService* fields_{nullptr};
    std::string status_;
};
} // namespace orbit::studio_ui

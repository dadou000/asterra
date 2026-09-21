#pragma once

#include <orbit/editor_model/CelestialAuthoringModel.hpp>
#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/studio_session/StudioSession.hpp>

#include <string>
#include <vector>

namespace orbit::studio_ui
{
class CelestialAuthoringUi
{
public:
    explicit CelestialAuthoringUi(
        studio_session::StudioSession& session);

    void Register(editor_ui::EditorUi& ui);

    inline static constexpr editor_ui::PanelId kPanel{
        .high = 0x4f52424954535455ULL,
        .low = 0x43454c4553544941ULL
    };

private:
    void Draw(editor_ui::PanelContext& context);

    studio_session::StudioSession& session_;
    std::string newSystemName_{"Celestial System"};
    std::string newBodyName_{"Celestial Body"};
    std::string newReferenceName_{"Barycenter"};
    std::vector<editor_model::CelestialDiagnostic> diagnostics_;
    std::string status_;
};
} // namespace orbit::studio_ui

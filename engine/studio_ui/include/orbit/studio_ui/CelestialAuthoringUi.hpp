#pragma once

#include <orbit/editor_model/CelestialAuthoringModel.hpp>
#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/studio_ui/StudioInspectorExtension.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

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

    // The same authoring surface can be hosted inside the contextual
    // Properties inspector. Keeping one draw path prevents the standalone
    // expert panel and the progressive-disclosure inspector from drifting.
    void Draw(editor_ui::PanelContext& context);

    [[nodiscard]] bool RelevantToSelection() const
    {
        auto& world = session_.World();
        if (!world.HasWorld())
        {
            return false;
        }

        editor_model::CelestialAuthoringModel model(
            world.Objects(),
            world.Schemas(),
            world.Commands(),
            world.Selection());

        const auto selected =
            model.PrimarySelection();

        if (!selected.has_value())
        {
            return false;
        }

        return selected->type ==
                   world_model::kCelestialSystemType ||
               selected->type ==
                   world_model::kCelestialBodyType ||
               selected->type ==
                   world_model::kCelestialReferenceNodeType ||
               selected->type ==
                   world_model::kRingBandType ||
               model.IsCapabilityType(
                   selected->type);
    }

    inline static constexpr editor_ui::PanelId kPanel{
        .high = 0x4f52424954535455ULL,
        .low = 0x43454c4553544941ULL
    };

private:
    studio_session::StudioSession& session_;
    std::string newSystemName_{"Celestial System"};
    std::string newBodyName_{"Celestial Body"};
    std::string newReferenceName_{"Barycenter"};
    i64 recipeSeed_{1};
    i64 recipePlanetCount_{4};
    bool recipeGenerateMoons_{true};
    std::string recipeSystemName_{"Generated System"};
    std::string recipeStarName_{"Primary"};
    std::vector<editor_model::CelestialDiagnostic> diagnostics_;
    std::string status_;

    StudioInspectorProviderRegistration inspectorProvider_{
        InspectorProviderDefinition{
            .id = "orbit.celestial-authoring",
            .owner = "orbit",
            .title = "Celestial Tools",
            .order = 110,
            .defaultOpen = false,
            .relevant =
                [this]()
                {
                    return RelevantToSelection();
                },
            .draw =
                [this](editor_ui::PanelContext& context)
                {
                    Draw(context);
                }
        }};
};
} // namespace orbit::studio_ui

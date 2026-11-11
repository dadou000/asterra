#pragma once

#include <orbit/editor_model/SurfaceAuthoringModel.hpp>
#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/studio_session/StudioWorkspace.hpp>
#include <orbit/studio_ui/StudioInspectorExtension.hpp>

#include <optional>
#include <string>
#include <unordered_map>

namespace orbit::studio_ui
{
class SurfaceAuthoringUi
{
public:
    explicit SurfaceAuthoringUi(
        studio_session::StudioWorkspace& workspace);
    explicit SurfaceAuthoringUi(
        studio_session::StudioSession& session);

    void Register(editor_ui::EditorUi& ui);

    // Shared draw path for the standalone expert panel and the contextual
    // Properties inspector. Progressive disclosure is decided by the host.
    void Draw(editor_ui::PanelContext& context);

    // Surface tools belong in the contextual inspector only when selection can
    // resolve to a terrain-bearing body. This intentionally includes terrain
    // descendants (biomes, constraints, process records, etc.) so selection
    // never kicks the user out of the same authoring workflow.
    [[nodiscard]] bool RelevantToSelection() const
    {
        studio_session::StudioSession* session =
            session_;

        if (workspace_ != nullptr)
        {
            if (!workspace_->HasProject())
            {
                return false;
            }

            session = &workspace_->Session();
        }

        if (session == nullptr ||
            !session->World().HasWorld())
        {
            return false;
        }

        auto& world = session->World();
        editor_model::SurfaceAuthoringModel model(
            world.Objects(),
            world.Commands(),
            world.Selection());

        return model.SelectedRockyBody().has_value();
    }

    // Used by the real-device Studio smoke gate to render the complete
    // advanced authoring surface deterministically. Normal Studio leaves this
    // disabled and preserves the user's progressive-disclosure choices.
    void SetAutomationCoverageMode(
        bool enabled) noexcept;

    inline static constexpr editor_ui::PanelId kPanel{
        .high = 0x4f52424954535455ULL,
        .low = 0x5355524641434534ULL
    };

private:
    studio_session::StudioWorkspace* workspace_{nullptr};
    studio_session::StudioSession* session_{nullptr};
    std::optional<scene::ObjectId> selectedBiome_;
    std::string newBiomeName_{"Desert"};
    std::string impactHistoryDraft_;
    scene::ObjectId impactHistoryDraftTerrain_{};
    std::unordered_map<scene::ObjectId, std::string> impactHistoryDrafts_;
    std::string stratigraphyDraft_;
    scene::ObjectId stratigraphyDraftTerrain_{};
    std::unordered_map<scene::ObjectId, std::string> stratigraphyDrafts_;

    math::Double3 localOverrideDirection_{0.0, 1.0, 0.0};
    f64 localOverrideInnerRadiusMeters_{250.0};
    f64 localOverrideOuterRadiusMeters_{1'000.0};
    f64 localOverrideWeight_{1.0};
    f64 localOverrideOpacity_{1.0};

    bool advancedBiome_{false};
    bool advancedProcesses_{false};
    bool automationCoverageMode_{false};
    std::string status_;

    StudioInspectorProviderRegistration inspectorProvider_{
        InspectorProviderDefinition{
            .id = "orbit.surface-authoring",
            .owner = "orbit",
            .title = "Surface Tools",
            .order = 100,
            .defaultOpen = true,
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

#pragma once

#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/studio_ui/StudioInspectorExtension.hpp>
#if defined(ORBIT_ENABLE_VALIDATION_TOOLS)
#include <orbit/validation/SceneValidationScenarios.hpp>
#endif
#include <orbit/volume_fields/VolumeFieldStorage.hpp>
#include <orbit/volume_solver/SurfaceVolumeSolver.hpp>
#include <orbit/world_model/VolumeSchemas.hpp>

#include <string>

namespace orbit::studio_ui
{
class StudioViewportRenderer;

class VolumeAuthoringUi
{
public:
    VolumeAuthoringUi(
        studio_session::StudioSession& session,
        volume_fields::VolumeFieldStorageService& fields,
        volume_solver::SurfaceVolumeSolverService& solver,
        StudioViewportRenderer& renderer) noexcept;

    void Register(
        editor_ui::EditorUi& ui);

    // Shared draw path for contextual Properties hosting. The standalone
    // Volumes panel remains available for dedicated expert workflows.
    void Draw(
        editor_ui::PanelContext& context);

    // Orbit Studio owns one production VolumeAuthoringUi at a time. The
    // contextual Inspector uses this live instance so it shares the exact
    // field-storage, solver, renderer, cache and validation state instead of
    // constructing a second authoring stack.
    [[nodiscard]] static VolumeAuthoringUi* ContextInstance() noexcept
    {
        return contextInstance_;
    }

    // True when the current single selection is a Volume domain or one of its
    // direct Source/Effector children. This is the same selection contract
    // used by the dedicated Volumes panel.
    [[nodiscard]] bool RelevantToSelection() const
    {
        if (session_ == nullptr ||
            !session_->World().HasWorld())
        {
            return false;
        }

        auto& world = session_->World();
        const auto& selected =
            world.Selection().Ordered();

        if (selected.size() != 1U)
        {
            return false;
        }

        const auto record =
            world.Objects().Find(
                selected.front());

        if (!record.has_value())
        {
            return false;
        }

        if (record->type ==
            world_model::kVolumeType)
        {
            return true;
        }

        if ((record->type ==
                 world_model::kVolumeSourceType ||
             record->type ==
                 world_model::kVolumeEffectorType) &&
            record->parent.has_value())
        {
            const auto parent =
                world.Objects().Find(
                    *record->parent);

            return parent.has_value() &&
                   parent->type ==
                       world_model::kVolumeType;
        }

        return false;
    }

    inline static constexpr editor_ui::PanelId kPanel{
        .high = 0x4f52424954535455ULL,
        .low = 0x564f4c554d455330ULL
    };

private:
    // Preserved M30-M35 implementation. M36 wraps these methods instead of
    // duplicating or rewriting the existing Volumes authoring workflow.
    void RegisterBase(
        editor_ui::EditorUi& ui);
    void DrawBase(
        editor_ui::PanelContext& context);

    void DrawRepresentationPolicy(
        editor_ui::PanelContext& context);

    studio_session::StudioSession* session_{nullptr};
    volume_fields::VolumeFieldStorageService* fields_{nullptr};
    volume_solver::SurfaceVolumeSolverService* solver_{nullptr};
    StudioViewportRenderer* renderer_{nullptr};

    inline static VolumeAuthoringUi* contextInstance_{nullptr};

    // Header-level lifetime bridge: no global service locator and no Main.cpp
    // wiring is required. The pointer is valid only while the production
    // authoring surface exists and is cleared automatically on destruction.
    struct ContextRegistration
    {
        VolumeAuthoringUi* owner{nullptr};

        explicit ContextRegistration(
            VolumeAuthoringUi* value) noexcept
            : owner(value)
        {
            VolumeAuthoringUi::contextInstance_ =
                value;
        }

        ~ContextRegistration()
        {
            if (VolumeAuthoringUi::contextInstance_ ==
                owner)
            {
                VolumeAuthoringUi::contextInstance_ =
                    nullptr;
            }
        }
    };

    ContextRegistration contextRegistration_{this};

    StudioInspectorProviderRegistration inspectorProvider_{
        InspectorProviderDefinition{
            .id = "orbit.volume-authoring",
            .owner = "orbit",
            .title = "Volume Tools",
            .order = 120,
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

    // M43 extends the normal Studio command catalog/palette with the named
    // validation scenarios. Member order follows session_/renderer_ so the
    // registration sees the active production services during construction.
#if defined(ORBIT_ENABLE_VALIDATION_TOOLS)
    SceneValidationCommandRegistration validationCommands_{
        session_,
        renderer_};
#endif

    math::Double3 paintPosition_{};
    f64 paintRadius_{2.0};
    f64 paintStrength_{1.0};

    // M37 native cache authoring/import state. Cache data itself lives in the
    // shared VolumeCacheRegistry and therefore remains renderer-accessible.
    u32 cacheBakeResolution_{32U};
    std::string cachePath_;
    std::string status_;
};
} // namespace orbit::studio_ui

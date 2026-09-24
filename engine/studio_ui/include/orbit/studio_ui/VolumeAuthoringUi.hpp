#pragma once

#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/studio_ui/V007ValidationScenarios.hpp>
#include <orbit/volume_fields/VolumeFieldStorage.hpp>
#include <orbit/volume_solver/SurfaceVolumeSolver.hpp>

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

    void Draw(
        editor_ui::PanelContext& context);
    void DrawRepresentationPolicy(
        editor_ui::PanelContext& context);

    studio_session::StudioSession* session_{nullptr};
    volume_fields::VolumeFieldStorageService* fields_{nullptr};
    volume_solver::SurfaceVolumeSolverService* solver_{nullptr};
    StudioViewportRenderer* renderer_{nullptr};

    // M43 extends the normal Studio command catalog/palette with the named
    // validation scenarios. Member order follows session_/renderer_ so the
    // registration sees the active production services during construction.
    V007ValidationCommandRegistration validationCommands_{
        session_,
        renderer_};

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

#pragma once

#include <orbit/editor_model/SystemViewModel.hpp>
#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/studio_session/StudioSession.hpp>

#include <optional>
#include <string>

namespace orbit::studio_ui
{
class SystemViewUi
{
public:
    explicit SystemViewUi(studio_session::StudioSession& session);
    void Register(editor_ui::EditorUi& ui);

    inline static constexpr editor_ui::PanelId kPanel{
        .high = 0x4f52424954535455ULL,
        .low = 0x53595354454d5657ULL
    };

private:
    enum class GizmoDragKind : u8
    {
        None = 0,
        Periapsis = 1,
        Apoapsis = 2,
        ReferencePosition = 3
    };

    void Draw(editor_ui::PanelContext& context);

    studio_session::StudioSession& session_;
    std::optional<scene::ObjectId> selectedSystem_;
    i64 editedTimeMicroseconds_{0};
    f64 editedRate_{1.0};
    bool timeFieldsInitialized_{false};
    std::optional<scene::ObjectId> focusedObject_;
    f64 viewZoom_{1.0};
    GizmoDragKind activeGizmo_{GizmoDragKind::None};
    std::optional<editor_model::OrbitManipulationTarget> orbitDraft_;
    std::optional<math::Double3> referenceDraft_;
    f64 dragBasePhysicalRadius_{0.0};
    f64 dragBaseScreenRadiusPixels_{0.0};
    std::string status_;
};
} // namespace orbit::studio_ui

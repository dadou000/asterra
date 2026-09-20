#pragma once

#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/studio_session/StudioWorkspace.hpp>

#include <optional>
#include <string>

namespace orbit::studio_ui
{
class SurfaceAuthoringUi
{
public:
    explicit SurfaceAuthoringUi(
        studio_session::StudioWorkspace& workspace);

    void Register(editor_ui::EditorUi& ui);

    inline static constexpr editor_ui::PanelId kPanel{
        .high = 0x4f52424954535455ULL,
        .low = 0x5355524641434534ULL
    };

private:
    void Draw(editor_ui::PanelContext& context);

    studio_session::StudioWorkspace* workspace_{nullptr};
    std::optional<scene::ObjectId> selectedBiome_;
    std::string newBiomeName_{"Desert"};

    math::Double3 localOverrideDirection_{0.0, 1.0, 0.0};
    f64 localOverrideInnerRadiusMeters_{250.0};
    f64 localOverrideOuterRadiusMeters_{1'000.0};
    f64 localOverrideWeight_{1.0};
    f64 localOverrideOpacity_{1.0};

    bool advancedBiome_{false};
    bool advancedProcesses_{false};
    std::string status_;
};
} // namespace orbit::studio_ui

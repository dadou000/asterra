#include <orbit/studio_ui/StudioExpansionShell.hpp>

#include <orbit/content/ContentService.hpp>
#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/editor_model/SurfaceAuthoringModel.hpp>
#include <orbit/editor_ui/FocusState.hpp>
#include <orbit/editor_ui/PanelExtensions.hpp>
#include <orbit/paths/PathNetwork.hpp>
#include <orbit/studio_ui/CommandPaletteModel.hpp>
#include <orbit/studio_ui/SelectionBreadcrumbs.hpp>
#include <orbit/studio_ui/StudioViewportPanels.hpp>
#include <orbit/studio_ui/VolumeAuthoringUi.hpp>
#include <orbit/terrain_debug/TerrainDebugField.hpp>
#include <orbit/terrain_debug/TerrainDebugSeam.hpp>
#include <orbit/world_model/VisibilityProxyBinding.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <format>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "StudioShellInternals.hpp"

namespace orbit::studio_ui
{
using namespace shell_detail;

void StudioExpansionShell::SavePersistentStateIfChanged() noexcept
{
    if (!persistentStateLoaded_ ||
        persistentStatePath_.empty())
    {
        return;
    }

    persistentState_.viewport = viewportState_;
    if (owner_ != nullptr)
    {
        persistentState_.inspectorAdvanced =
            owner_->contextualAdvancedProperties_;
    }

    const std::string serialized =
        SerializeStudioPersistentState(
            persistentState_);

    if (serialized == persistentSnapshot_)
    {
        return;
    }

    try
    {
        SaveStudioPersistentState(
            persistentStatePath_,
            persistentState_);
        persistentSnapshot_ = serialized;
    }
    catch (const std::exception& exception)
    {
        if (owner_ != nullptr)
        {
            owner_->status_ =
                std::string{"Studio state save failed: "} +
                exception.what();
        }
    }
}

void StudioExpansionShell::SyncPersistentState() noexcept
{
    studio_session::StudioSession* const session =
        owner_ != nullptr
            ? owner_->session_
            : nullptr;

    if (session == persistentSession_ &&
        persistentStateLoaded_)
    {
        SavePersistentStateIfChanged();
        return;
    }

    // Rebinding projects is a normal Studio operation. Persist the old
    // project before swapping the presentation binding.
    SavePersistentStateIfChanged();

    persistentSession_ = session;
    persistentStatePath_.clear();
    persistentState_ = {};
    persistentSnapshot_.clear();
    persistentStateLoaded_ = false;

    if (session == nullptr)
    {
        return;
    }

    try
    {
        persistentStatePath_ =
            session->World().Project().RootDirectory() /
            ".orbit" /
            "StudioState.ini";

        persistentState_ =
            LoadStudioPersistentState(
                persistentStatePath_);

        viewportState_ = persistentState_.viewport;
        if (owner_ != nullptr)
        {
            owner_->contextualAdvancedProperties_ =
                persistentState_.inspectorAdvanced;
        }

        persistentSnapshot_ =
            SerializeStudioPersistentState(
                persistentState_);
        persistentStateLoaded_ = true;
    }
    catch (const std::exception& exception)
    {
        if (owner_ != nullptr)
        {
            owner_->status_ =
                std::string{"Studio state load failed: "} +
                exception.what();
        }
    }
}
} // namespace orbit::studio_ui

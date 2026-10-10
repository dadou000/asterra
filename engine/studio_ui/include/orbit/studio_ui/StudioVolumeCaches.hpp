#pragma once

#include <orbit/rpc/JsonRpc.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/studio_session/StudioSession.hpp>

#include <filesystem>
#include <string>

namespace orbit::studio_ui
{
// Attaches the .orbitvol named by each Volume's Baked Cache Path
// (world_model::kVolumeCachePath, relative to the project root) when the Volume
// has no cache attached yet, so a saved Weather Lab storm survives reopening
// the project. A file that fails to load is reported once per (volume, path)
// and then skipped. Returns the number of caches attached. Cheap enough to
// call every few dozen frames.
u32 SyncVolumeCaches(
    const scene::ObjectStore& objects,
    const std::filesystem::path& projectRoot,
    std::string* log = nullptr);

// volume.cache_save / volume.cache_load (docs/ORBIT_MCP.md). `session` must
// outlive the dispatcher.
void RegisterVolumeCacheRpc(
    rpc::Dispatcher& dispatcher,
    studio_session::StudioSession& session);
} // namespace orbit::studio_ui

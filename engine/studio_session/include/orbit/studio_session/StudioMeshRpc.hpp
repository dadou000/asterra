#pragma once

#include <orbit/rpc/JsonRpc.hpp>

namespace orbit::studio_session
{
class StudioSession;

// Registers `mesh.import`: validates a glTF/GLB source, copies it (and, for a
// .gltf, its sibling files) into the open project's Content/Models/<name>/
// folder and reports the project-relative asset path plus triangle count,
// bounds and import warnings. Place the result with object.create (type
// "Static Mesh") and property.set, or the orbit_mesh_import MCP tool, which
// does both. Also registers `primitive.create`, which authors a visible
// Primitive (box / sphere / cylinder / capsule / plane, standard / mirror /
// glass surface) in one undo step. StudioSession registers both from its own
// constructor.
void RegisterStudioMeshRpc(
    rpc::Dispatcher& dispatcher,
    StudioSession& session);
} // namespace orbit::studio_session

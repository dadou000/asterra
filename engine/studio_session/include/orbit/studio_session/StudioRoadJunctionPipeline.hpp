#pragma once
#include <orbit/path_geometry/RoadJunctionPublication.hpp>
#include <orbit/studio_session/UniverseBoundPathCache.hpp>
#include <orbit/paths/PathNetwork.hpp>
#include <orbit/editor_session/EditorWorldSession.hpp>
#include <map>
#include <string>
#include <vector>

namespace orbit::studio_session {
// Single cache-mediated publication stage for road-junction geometry,
// collision and navigation, with one immutable packet per reference frame.
// Raw UniverseBoundPathCache products remain the regeneration inputs.
class StudioRoadJunctionPipeline
{
public:
    void Refresh(editor_session::EditorWorldSession& world,
                 paths::PathNetworkService& network,
                 const UniverseBoundPathCache& paths);
    void Clear() noexcept;
    [[nodiscard]] const path_geometry::RoadJunctionPublishedEdge*
    FindEdge(scene::ObjectId edge) const noexcept;
    [[nodiscard]] const path_geometry::RoadJunctionPatchProduct*
    FindPatch(scene::ObjectId node) const noexcept;
    [[nodiscard]] std::vector<const path_geometry::RoadJunctionScenePacket*>
    Packets() const;
    [[nodiscard]] const std::string& LastError() const noexcept {
        return lastError_;
    }
    [[nodiscard]] u64 Revision() const noexcept {return revision_;}
    [[nodiscard]] std::size_t ActiveJunctions() const noexcept;
private:
    std::map<frames::FrameId,path_geometry::RoadJunctionScenePublisher> frames_;
    u64 observedFingerprint_{0U};
    u64 revision_{0U};
    std::string lastError_;
};
} // namespace orbit::studio_session

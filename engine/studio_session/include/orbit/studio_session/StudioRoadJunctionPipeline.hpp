#pragma once
#include <orbit/path_geometry/RoadJunctionPublication.hpp>
#include <orbit/path_geometry/RoadContactScene.hpp>
#include <orbit/path_geometry/RoadVehicleFixture.hpp>
#include <orbit/path_geometry/RoadVehicleWorld.hpp>
#include <orbit/studio_session/UniverseBoundPathCache.hpp>
#include <orbit/paths/PathNetwork.hpp>
#include <orbit/editor_session/EditorWorldSession.hpp>
#include <future>
#include <map>
#include <optional>
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
    // Contact scene and renderer use the SAME packet signature/frame only
    // while each authored source is current and no source is dirty.
    [[nodiscard]] const path_geometry::RoadContactSceneGeneration*
    CurrentContact(scene::ObjectId junction,
                   const UniverseBoundPathCache& raw) const noexcept;
    [[nodiscard]] const path_geometry::RoadContactSceneOwner*
    ActiveContactOwner(scene::ObjectId junction,
                       const UniverseBoundPathCache& raw) const noexcept;
    [[nodiscard]] std::size_t ActiveContactScenes() const noexcept;
    // Fails closed on a dirty source or unmatched packet generation.
    [[nodiscard]] std::optional<path_geometry::RoadContactHit>
    ProbeContact(scene::ObjectId junction,
                 const UniverseBoundPathCache& raw,
                 path_geometry::RoadContactProbe probe) const;
    [[nodiscard]] std::optional<path_geometry::RoadVehicleFixtureReport>
    RunVehicleFixture(scene::ObjectId junction,
                      const UniverseBoundPathCache& raw,
                      path_geometry::RoadVehicleFixtureRequest request) const;

    // Renderer-facing coherent view: trim affected raw corridors and include
    // the junction patches exactly once, leaving all other paths untouched.
    // Returned product pointers remain owned by the raw cache or this
    // pipeline and are valid through the current Studio render-graph frame.
    [[nodiscard]] std::vector<const path_geometry::PathDerivedProduct*>
    RenderProducts(const UniverseBoundPathCache& raw) const;

    [[nodiscard]] const std::string& LastError() const noexcept {
        return lastError_;
    }
    [[nodiscard]] u64 Revision() const noexcept {return revision_;}
    [[nodiscard]] std::size_t ActiveJunctions() const noexcept;
private:
    struct PreparedJunction
    {
        frames::FrameId frame{};
        path_geometry::RoadJunctionPublicationResult candidate;
        path_geometry::RoadContactSceneBuild cooked;
    };
    struct PreparedBatch
    {
        std::vector<PreparedJunction> junctions;
        std::string error;
    };
    struct PendingBuild
    {
        std::future<PreparedBatch> future;
        u64 fingerprint{0U};
        u64 worldGeneration{0U};
    };
    // Task futures are packaged by Orbit::Jobs, not std::async: erasing an
    // obsolete pending build cannot synchronously join the render thread.
    std::optional<PendingBuild> pendingBuild_;
    std::map<frames::FrameId,path_geometry::RoadJunctionScenePublisher> frames_;
    std::map<frames::FrameId,path_geometry::RoadContactSceneOwner> contactScenes_;
    [[nodiscard]] bool PacketCurrent(
        const path_geometry::RoadJunctionScenePacket& packet,
        const UniverseBoundPathCache& raw) const noexcept;
    std::map<scene::ObjectId,path_geometry::PathDerivedProduct> renderPatches_;
    u64 observedFingerprint_{0U};
    u64 revision_{0U};
    std::string lastError_;
};
} // namespace orbit::studio_session

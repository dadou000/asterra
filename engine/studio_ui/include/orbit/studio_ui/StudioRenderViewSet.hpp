#pragma once

#include <orbit/lighting/SurfaceDebugRenderer.hpp>
#include <orbit/render_view/RenderView.hpp>
#include <orbit/studio_session/StudioRuntimeBinding.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/studio_ui/StudioFlatMap.hpp>
#include <orbit/studio_ui/StudioSurfacePicking.hpp>
#include <orbit/studio_ui/StudioTerrainLayerOptions.hpp>
#include <orbit/studio_ui/StudioViewportTextDiagnostics.hpp>
#include <orbit/studio_ui/StudioViewportCamera.hpp>
#include <orbit/studio_ui/StudioViewportNavigation.hpp>
#include <orbit/terrain_debug/TerrainDebugField.hpp>

#include <map>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::studio_ui
{
enum class StudioTerrainOverlayKind : u8
{
    Brush,
    Spline
};

struct StudioTerrainAuthoringOverlay
{
    universe::BodyId body{};
    StudioTerrainOverlayKind kind{
        StudioTerrainOverlayKind::Brush};
    std::vector<math::Double3> controlUnitDirections;
    f64 influenceRadiusMeters{0.0};
};

// M13 diagnostics are transient view state. None of these flags may enter
// terrain identity, invalidation, persistence or generation contracts.
struct StudioTerrainDiagnosticOverlayOptions
{
    bool dirtyPageBounds{false};
    bool buildStates{false};
    bool physicalLod{false};
    // Outlines of the ACTIVE clipmap levels only (the dynamic planner's range),
    // finest and coarsest drawn brighter.
    bool clipmapRings{false};
    // Tints the terrain surface by clipmap level (a shading change, not lines)
    // so the active levels and where each one hands off are visible.
    bool clipmapLevels{false};
    // Colours each clipmap vertex by the health of its GPU sample: red = elevation not finite or
    // beyond +-20 km, green = morph target not finite or far from the vertex, blue = slope not
    // finite or steeper than 20, grey = healthy. Rows of one colour are a corrupted strip.
    bool clipmapSampleHealth{false};
    // Hole / fade view: nothing is culled; each vertex is coloured by why it would be (red beyond the
    // horizon, green inside a finer level's hole, blue inside it while that level fades in, cyan culled by
    // distance bands, grey drawn normally), to see gaps where a finer level is missing.
    bool clipmapHoleView{false};
    // Projected position view: nothing is culled; each vertex is coloured by where its clip position
    // lands (red non-finite, green behind the camera, blue outside the near/far range, cyan off screen
    // sideways, grey on screen), to find triangles the GPU clips away.
    bool clipmapProjectionView{false};
    // Shading view: each terrain pixel is coloured by which interpolated input is bad (yellow biome
    // weights sum to zero, magenta non-finite position, red/green/blue terrain normal, body-fixed
    // normal, surface direction), written as emission so lighting cannot hide it. Dark grey = fine.
    bool clipmapShadingView{false};
    // Draws the terrain clipmap as a wireframe (the water surface is hidden).
    bool clipmapWireframe{false};
    // Freezes the clipmap where it is: plan, window and content stop following
    // the camera, which can then fly away and look at the rings from outside.
    bool clipmapFreeze{false};
    bool cacheStatus{false};
    bool authoredConstraints{false};
    bool biomeWeights{false};
    bool processMasks{false};
    bool drainageVectors{false};

    [[nodiscard]] constexpr bool Any() const noexcept
    {
        return
            dirtyPageBounds ||
            buildStates ||
            physicalLod ||
            clipmapRings ||
            cacheStatus ||
            authoredConstraints ||
            biomeWeights ||
            processMasks ||
            drainageVectors;
    }

    [[nodiscard]] constexpr bool operator==(
        const StudioTerrainDiagnosticOverlayOptions&) const noexcept = default;
};

struct StudioViewPose
{
    // scene::ObjectId::ToString of the viewport's target body.
    std::string targetObject;
    // False for a view that has a target but has not been navigated yet
    // (reference-sphere bodies): the pose below is then meaningless and only
    // the target is worth restoring.
    bool hasCamera{true};
    // True when the target has a terrain runtime (terrain navigation); false
    // for the zero-elevation reference sphere.
    bool terrain{false};
    math::Double3 observerMeters{};
    world::SurfaceFrame surfaceFrame{};
    f64 yawRadians{0.0};
    f64 pitchRadians{0.0};
    // Camera zoom of the view (1 = the default field of view).
    f64 zoom{1.0};
};

// One tile of a stitched high-resolution capture: the camera is turned by yaw
// (about its up axis) then pitch (about its resulting right axis) and given a
// narrower field of view, so a series of tiles can be reprojected into one
// large image of the original camera.
struct StudioCaptureTile
{
    f64 yawRadians{0.0};
    f64 pitchRadians{0.0};
    f64 verticalFovRadians{1.0};
};

struct StudioRenderViewInfo
{
    std::string id;
    studio_session::ViewportMode mode{
        studio_session::ViewportMode::Perspective};
    u32 width{1};
    u32 height{1};
    bool hasTarget{false};
    lighting::SurfaceDebugMode surfaceDebugMode{
        lighting::SurfaceDebugMode::Lit};
    terrain_debug::TerrainDebugField debugField{
        terrain_debug::TerrainDebugField::Uplift};
    u8 debugPhysicalPageLevel{8};
    std::optional<StudioPhysicalPageSelection> debugPhysicalPage;
    bool hasLiveDebugPage{false};
    StudioTerrainDiagnosticOverlayOptions diagnostics{};
    StudioTerrainLayerOptions layers{};
    FlatMapLayer flatMapLayer{FlatMapLayer::Elevation};
};

// Owns the actual resizable GPU RenderViews corresponding to logical Studio
// viewport target slots. Target state remains in StudioSession; this class owns
// only presentation resources and never persists BodyId/FrameId authority.
class StudioRenderViewSet
{
public:
    StudioRenderViewSet(
        rhi::Device& device,
        studio_session::StudioSession& session) noexcept;
    ~StudioRenderViewSet();

    StudioRenderViewSet(
        const StudioRenderViewSet&) = delete;
    StudioRenderViewSet& operator=(
        const StudioRenderViewSet&) = delete;

    void CreateDefaults();

    void Create(
        std::string id,
        studio_session::ViewportMode mode,
        bool followActiveBody,
        render_view::RenderViewDesc desc);

    [[nodiscard]] bool Destroy(
        std::string_view id) noexcept;

    void Resize(
        std::string_view id,
        u32 width,
        u32 height);

    void SetCompositionEnabled(
        std::string_view id,
        bool enabled);

    [[nodiscard]] bool CompositionEnabled(
        std::string_view id) const noexcept;

    void SetNavigationSpeedScale(
        std::string_view id,
        f64 scale);

    [[nodiscard]] f64 NavigationSpeedScale(
        std::string_view id) const;

    // M05 production-terrain navigation. These mutate only the M03 observer
    // and transient camera state; terrain authority/revisions stay untouched.
    [[nodiscard]] bool NavigateTerrain(
        std::string_view id,
        const StudioTerrainNavigationInput& input);

    // True when the viewport currently has a usable terrain runtime, i.e.
    // NavigateTerrain drives the terrain observer rather than the reference
    // sphere.
    [[nodiscard]] bool HasTerrainNavigation(std::string_view id) const;

    // Same navigation for a body with no terrain runtime (no Surface
    // capability, a star, ...), run against a zero-elevation reference sphere.
    [[nodiscard]] bool NavigateReference(
        std::string_view id,
        const StudioTerrainNavigationInput& input);

    [[nodiscard]] bool FocusTerrainBody(
        std::string_view id);

    // Frames body-fixed selection bounds through the persistent navigation
    // pose. Refresh advances the two-second camera move.
    [[nodiscard]] bool FrameSelectedBounds(
        std::string_view id,
        const math::Double3& centerMeters,
        f64 radiusMeters);

    [[nodiscard]] bool FocusTerrainSurfacePoint(
        std::string_view id,
        f32 u,
        f32 v);

    [[nodiscard]] bool ResetTerrainView(
        std::string_view id);

    // Camera zoom: a telephoto factor on the view's field of view (2 shows
    // half the angle, 0.5 twice as wide). Presentation state like the debug
    // field: it never enters terrain authority, and it is applied to the
    // camera each Refresh so picking and every renderer agree on it.
    // Vertical field of view of the camera the view renders with now (zoom
    // included).
    [[nodiscard]] f64 ViewFovRadians(std::string_view id) const;

    // Capture tiling (see StudioCaptureTile). Applied each Refresh on top of
    // the zoomed camera; nullopt returns to the normal camera.
    void SetCaptureTile(
        std::string_view id,
        const std::optional<StudioCaptureTile>& tile);

    static constexpr f64 kMinZoom = 0.5;
    static constexpr f64 kMaxZoom = 100.0;
    void SetZoom(std::string_view id, f64 zoom);
    [[nodiscard]] f64 Zoom(std::string_view id) const;

    // The complete camera pose of a perspective view: the observer in the
    // planet-fixed frame, the surface frame it travels with and the free-camera
    // look angles. Issue reports capture it so a situation can be recreated
    // exactly (RestoreViewPose). Absent when the view has no target body.
    [[nodiscard]] std::optional<StudioViewPose> ViewPose(
        std::string_view id) const;

    // Puts the camera back on a captured pose. The target body must already be
    // the one the pose was captured against (pose.targetObject); returns false
    // when it is not, or when there is no usable view.
    [[nodiscard]] bool RestoreViewPose(
        std::string_view id,
        const StudioViewPose& pose);

    // M08 shared production-terrain surface pick. All viewport authoring tools
    // consume this seam instead of deriving their own cube/ray identity.
    [[nodiscard]] std::optional<StudioSurfacePick>
    PickTerrainSurface(
        std::string_view id,
        f32 u,
        f32 v,
        std::optional<u8> physicalTileLevel = std::nullopt);

    // Complete numeric/text diagnostic for one view: camera, heights (above
    // terrain, water surface, datum and from the planet core), and the terrain
    // source's climate/biome/water/slope at the point below the camera and, when
    // a cursor position in view UV is given, under the cursor. Backs the viewport
    // HUD and the view.text_diagnostics RPC.
    void SetTerrainLayers(
        std::string_view id,
        StudioTerrainLayerOptions options);

    [[nodiscard]] StudioTerrainLayerOptions TerrainLayers(
        std::string_view id) const;

    // The elevation of the terrain the clipmap actually draws under a view's
    // camera (read back from the GPU a few frames late). Terrain navigation uses
    // it as a floor next to the CPU terrain source, so the camera cannot end up
    // under what is on screen. Published by the renderer each frame.
    void SetRenderedGround(
        std::string_view id,
        const math::Double3& unitDirection,
        f64 elevationMeters);

    // The renderer owns the orbital patch builds, so the host publishes their
    // counters here each frame for the diagnostics report.
    // What the clipmap planner chose for a view; set by the frame loop.
    // The target body's cloud field, published by the host each frame.
    void SetCloudReport(
        std::string_view id,
        const std::optional<StudioCloudReport>& report);
    void SetClipmapPlanStats(
        std::string_view id,
        const StudioClipmapPlanStats& stats);

    void SetOrbitalPatchStats(
        std::string_view id,
        u32 patchesPending,
        u32 patchesResident);

    // Whether the viewport draws its text diagnostics readout. Transient
    // presentation state, like the debug field.
    void SetTextDiagnosticsHud(std::string_view id, bool enabled);
    [[nodiscard]] bool TextDiagnosticsHud(std::string_view id) const;

    [[nodiscard]] StudioViewportTextReport TextDiagnostics(
        std::string_view id,
        std::optional<std::pair<f32, f32>> cursorUv = std::nullopt);

    [[nodiscard]] std::optional<StudioSurfacePick>
    LastTerrainSurfacePick(
        std::string_view id) const;

    void SetTerrainAuthoringOverlay(
        std::string_view id,
        StudioTerrainAuthoringOverlay overlay);

    void ClearTerrainAuthoringOverlay(
        std::string_view id) noexcept;

    [[nodiscard]] std::optional<
        StudioTerrainAuthoringOverlay>
    TerrainAuthoringOverlay(
        std::string_view id) const;

    void SetTerrainDiagnosticOverlays(
        std::string_view id,
        StudioTerrainDiagnosticOverlayOptions options);

    [[nodiscard]] StudioTerrainDiagnosticOverlayOptions
    TerrainDiagnosticOverlays(
        std::string_view id) const;

    // Debug-field choice is transient RenderView presentation state. It is
    // deliberately excluded from StudioSession/project terrain authority.
    void SetDebugField(
        std::string_view id,
        terrain_debug::TerrainDebugField field);

    [[nodiscard]] terrain_debug::TerrainDebugField
    DebugField(std::string_view id) const;

    // GBuffer-channel debug view. Transient RenderView presentation state,
    // same as SetDebugField above.
    void SetSurfaceDebugMode(
        std::string_view id,
        lighting::SurfaceDebugMode mode);

    [[nodiscard]] lighting::SurfaceDebugMode
    SurfaceDebugMode(std::string_view id) const;

    // Which layer the flat map of a view shows (Flat Map viewport mode).
    void SetFlatMapLayer(
        std::string_view id,
        FlatMapLayer layer);

    [[nodiscard]] FlatMapLayer FlatMapLayerOf(
        std::string_view id) const;

    // The flat map's progress and marker, pushed in by the host each frame.
    void SetFlatMapStatus(
        std::string_view id,
        const std::optional<StudioFlatMapStatus>& status);

    [[nodiscard]] std::optional<StudioFlatMapStatus> FlatMapStatusOf(
        std::string_view id) const;

    // Moves the terrain observer of a view to the surface point in the given
    // direction (unit vector in the planet's body-fixed frame), exactly like
    // double-clicking that point on the terrain. Returns false when the view
    // has no current terrain runtime.
    [[nodiscard]] bool FocusTerrainDirection(
        std::string_view id,
        const math::Double3& unitDirection);

    // The point of the planet under a viewport position (u, v in 0..1, origin
    // top-left) in a view that shows the planet as a map: the flat map
    // (flat_map mode) or the globe (body_map mode). Returns the unit direction
    // in the planet's body-fixed frame, or nullopt when the position misses the
    // map or planet (letterbox bars, space) or the view shows neither.
    [[nodiscard]] std::optional<math::Double3> PickPlanetDirection(
        std::string_view id,
        f32 u,
        f32 v) const;

    // Switches the viewport mode of a view (the mode selector's operation).
    void SetViewportMode(
        std::string_view id,
        studio_session::ViewportMode mode);

    // Width and height of a view's image, or nullopt for an unknown view.
    [[nodiscard]] std::optional<std::pair<u32, u32>> ViewSize(
        std::string_view id) const;

    void SetDebugPhysicalPageLevel(
        std::string_view id,
        u8 level);

    [[nodiscard]] u8 DebugPhysicalPageLevel(
        std::string_view id) const;

    [[nodiscard]] bool SelectDebugPhysicalPage(
        std::string_view id,
        f32 u,
        f32 v);

    [[nodiscard]] std::optional<StudioPhysicalPageSelection>
    DebugPhysicalPage(std::string_view id) const;

    [[nodiscard]] std::shared_ptr<
        const terrain_debug::TerrainDebugPageData>
    LiveDebugPage(std::string_view id) const;

    // Applies generation-validated target cameras to every GPU view. Missing
    // targets are normal for blank worlds and leave a neutral unbound camera.
    [[nodiscard]] u32 Refresh(
        const studio_session::StudioRuntimeSnapshot& snapshot);

    [[nodiscard]] render_view::RenderView* Find(
        std::string_view id) noexcept;
    [[nodiscard]] const render_view::RenderView* Find(
        std::string_view id) const noexcept;

    [[nodiscard]] std::vector<StudioRenderViewInfo>
    Catalog() const;

private:
    void RequireCurrentSnapshot(
        const studio_session::StudioRuntimeSnapshot& snapshot) const;

    [[nodiscard]] StudioTerrainNavigationState&
    RequireNavigationState(
        std::string_view id);

    [[nodiscard]] const StudioTerrainNavigationState&
    RequireNavigationState(
        std::string_view id) const;

    rhi::Device* device_{nullptr};
    studio_session::StudioSession* session_{nullptr};
    std::map<
        std::string,
        std::unique_ptr<render_view::RenderView>,
        std::less<>>
        views_;

    std::map<
        std::string,
        StudioTerrainNavigationState,
        std::less<>>
        navigationStates_;

    // Navigation for a body that has no terrain runtime. It runs the exact
    // terrain navigation math (surface-relative motion, altitude-scaled speed,
    // look) against a zero-elevation reference sphere, so both kinds of body
    // move identically. It persists per view until the viewport target
    // changes (selecting another body reframes it) or the view is reset.
    struct ReferenceNavigation
    {
        scene::ObjectId target{};
        u64 universeGeneration{0U};
        StudioTerrainNavigationState navigation{};
        world::WorldPosition observer{};
    };

    std::map<std::string, ReferenceNavigation, std::less<>>
        referenceNavigation_;

    struct FrameTransition
    {
        StudioViewPose start;
        math::Double3 targetObserver{};
        math::Double3 targetCenter{};
        std::chrono::steady_clock::time_point started{};
    };
    std::map<std::string, FrameTransition, std::less<>> frameTransitions_;

    std::map<std::string, bool, std::less<>> compositionEnabled_;
    std::map<std::string, bool, std::less<>> textDiagnosticsHud_;
    std::map<std::string, f64, std::less<>> zoom_;
    std::map<std::string, StudioCaptureTile, std::less<>> captureTiles_;

    std::map<std::string, StudioTerrainLayerOptions, std::less<>>
        terrainLayers_;
    std::map<std::string, StudioCloudReport, std::less<>> cloudReports_;

    struct RenderedGround
    {
        math::Double3 unitDirection{};
        f64 elevationMeters{0.0};
    };
    std::map<std::string, RenderedGround, std::less<>> renderedGround_;

    struct OrbitalPatchStats
    {
        u32 pending{0U};
        u32 resident{0U};
    };
    std::map<std::string, OrbitalPatchStats, std::less<>> orbitalPatchStats_;
    std::map<std::string, StudioClipmapPlanStats, std::less<>> clipmapPlanStats_;

    std::map<
        std::string,
        terrain_debug::TerrainDebugField,
        std::less<>>
        debugFields_;

    std::map<
        std::string,
        u8,
        std::less<>>
        debugPhysicalPageLevels_;

    std::map<std::string, FlatMapLayer, std::less<>> flatMapLayers_;
    std::map<std::string, StudioFlatMapStatus, std::less<>> flatMapStatuses_;

    std::map<
        std::string,
        StudioPhysicalPageSelection,
        std::less<>>
        debugPhysicalPages_;

    std::map<
        std::string,
        StudioSurfacePick,
        std::less<>>
        terrainSurfacePicks_;

    std::map<
        std::string,
        StudioTerrainAuthoringOverlay,
        std::less<>>
        terrainAuthoringOverlays_;

    std::map<
        std::string,
        StudioTerrainDiagnosticOverlayOptions,
        std::less<>>
        terrainDiagnosticOverlays_;

    std::map<
        std::string,
        std::shared_ptr<const terrain_debug::TerrainDebugPageData>,
        std::less<>>
        liveDebugPages_;
};
} // namespace orbit::studio_ui

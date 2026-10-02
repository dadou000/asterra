#pragma once

#include <orbit/lighting/SurfaceDebugRenderer.hpp>
#include <orbit/render_view/RenderView.hpp>
#include <orbit/studio_session/StudioRuntimeBinding.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/studio_ui/StudioSurfacePicking.hpp>
#include <orbit/studio_ui/StudioTerrainLayerOptions.hpp>
#include <orbit/studio_ui/StudioViewportTextDiagnostics.hpp>
#include <orbit/studio_ui/StudioViewportCamera.hpp>
#include <orbit/studio_ui/StudioViewportNavigation.hpp>
#include <orbit/terrain_debug/TerrainDebugField.hpp>

#include <map>
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

    [[nodiscard]] bool FocusTerrainSurfacePoint(
        std::string_view id,
        f32 u,
        f32 v);

    [[nodiscard]] bool ResetTerrainView(
        std::string_view id);

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

    std::map<std::string, bool, std::less<>> compositionEnabled_;
    std::map<std::string, bool, std::less<>> textDiagnosticsHud_;

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

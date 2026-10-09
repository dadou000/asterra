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

void StudioExpansionShell::DrawInspectorExtension(
    editor_ui::PanelContext& context)
{
    SyncPersistentState();

    const auto providers =
        GlobalInspectorProviders().Relevant();

    if (providers.empty())
    {
        return;
    }

    context.Separator();
    context.MutedText("Contextual");
    static_cast<void>(
        GlobalInspectorProviders().DrawRelevant(
            context));
}

std::string_view StudioExpansionShell::SelectedViewportId() const noexcept
{
    if (viewportControlMode_ == 1)
    {
        return "studio.primary";
    }
    if (viewportControlMode_ == 2)
    {
        return "studio.map";
    }

    const std::string_view focused =
        editor_ui::FocusedWindowTitle();
    if (focused == "Viewport")
    {
        lastFocusedViewportIndex_ = 0;
    }
    else if (focused == "Body Map / Debug View")
    {
        lastFocusedViewportIndex_ = 1;
    }

    return lastFocusedViewportIndex_ == 1
        ? std::string_view{"studio.map"}
        : std::string_view{"studio.primary"};
}

bool StudioExpansionShell::ViewportControlsRelevant() const noexcept
{
    if (owner_ == nullptr ||
        owner_->views_ == nullptr ||
        owner_->session_ == nullptr ||
        !owner_->session_->World().HasWorld())
    {
        return false;
    }

    const std::string_view id = SelectedViewportId();
    return owner_->views_->Find(id) != nullptr &&
        owner_->session_->Viewports().Find(id) != nullptr;
}

bool StudioExpansionShell::BezierContextRelevant() const noexcept
{
    if (owner_ == nullptr ||
        owner_->session_ == nullptr ||
        !owner_->session_->World().HasWorld())
    {
        return false;
    }

    auto& paths = owner_->session_->PathNetwork().Service();
    for (const auto object :
         owner_->session_->World().Selection().Ordered())
    {
        const auto edge = paths.FindEdge(object);
        if (edge.has_value() &&
            edge->mode == paths::EdgeMode::Bezier)
        {
            return true;
        }
    }
    return false;
}

bool StudioExpansionShell::DrawBuiltInQuickCreate(
    editor_ui::PanelContext& context)
{
    if (owner_ == nullptr)
    {
        return false;
    }

    bool created = false;

    const std::string viewportId{SelectedViewportId()};
    const bool enabled =
        owner_->CanCreateAtViewport(viewportId);
    const std::string disabledReason =
        enabled
            ? std::string{}
            : std::string{
                "The controlled viewport must target a world body."};

    std::vector<editor_ui::ActionPresentation> actions;
    actions.reserve(4U);

    actions.push_back({
        .label = "Point Light##quick-add-point-light",
        .enabled = enabled,
        .disabledReason = disabledReason,
        .invoke =
            [this, viewportId, &created]
            {
                try
                {
                    owner_->CreateLocalLightAtViewport(
                        viewportId,
                        false);
                    created = true;
                }
                catch (const std::exception& exception)
                {
                    owner_->status_ = exception.what();
                }
            }
    });
    actions.push_back({
        .label = "Spot Light##quick-add-spot-light",
        .enabled = enabled,
        .disabledReason = disabledReason,
        .invoke =
            [this, viewportId, &created]
            {
                try
                {
                    owner_->CreateLocalLightAtViewport(
                        viewportId,
                        true);
                    created = true;
                }
                catch (const std::exception& exception)
                {
                    owner_->status_ = exception.what();
                }
            }
    });
    actions.push_back({
        .label = "Sphere Proxy##quick-add-sphere-proxy",
        .enabled = enabled,
        .disabledReason = disabledReason,
        .invoke =
            [this, viewportId, &created]
            {
                try
                {
                    owner_->CreateVisibilityProxyAtViewport(
                        viewportId,
                        false);
                    created = true;
                }
                catch (const std::exception& exception)
                {
                    owner_->status_ = exception.what();
                }
            }
    });
    actions.push_back({
        .label = "Box Proxy##quick-add-box-proxy",
        .enabled = enabled,
        .disabledReason = disabledReason,
        .invoke =
            [this, viewportId, &created]
            {
                try
                {
                    owner_->CreateVisibilityProxyAtViewport(
                        viewportId,
                        true);
                    created = true;
                }
                catch (const std::exception& exception)
                {
                    owner_->status_ = exception.what();
                }
            }
    });

    static_cast<void>(context.ActionList(actions));
    return created;
}

void StudioExpansionShell::DrawViewportTargetProperties(
    editor_ui::PanelContext& context)
{
    if (!ViewportControlsRelevant())
    {
        return;
    }

    viewportControlMode_ = std::clamp(viewportControlMode_, 0, 2);
    static_cast<void>(
        context.Combo(
            "Control##viewport-control-mode",
            kViewportControlModes,
            viewportControlMode_));

    static constexpr std::array<std::string_view, 4>
        kLayouts{"Single", "Vertical", "Horizontal", "Quad"};
    i32 layout = static_cast<i32>(viewportState_.layout);
    if (context.Combo(
            "Layout##viewport-layout-properties",
            kLayouts,
            layout))
    {
        viewportState_.SetLayout(
            static_cast<ViewportLayout>(layout));
    }

    static constexpr std::array<std::string_view, 2>
        kTransformSpaces{"World", "Local"};
    i32 transformSpace = static_cast<i32>(viewportState_.gizmo.space);
    if (context.Combo(
            "Transform Space##viewport-transform-space-properties",
            kTransformSpaces,
            transformSpace))
    {
        transformSpace = std::clamp(transformSpace, 0, 1);
        viewportState_.gizmo.space =
            static_cast<GizmoSpace>(transformSpace);
    }

    owner_->DrawSnappingSettings(context);

    const std::string_view id = SelectedViewportId();
    auto& session = *owner_->session_;
    const auto* target = session.Viewports().Find(id);

    context.Text(
        id == "studio.map"
            ? "Controlled viewport: Body Map / Debug View"
            : "Controlled viewport: Primary");
    context.MutedText(
        viewportControlMode_ == 0
            ? "Auto follows the last focused production viewport."
            : "Viewport control is pinned until Control returns to Auto.");
    context.MutedText(
        std::format(
            "Mode: {}",
            ViewportModeLabel(target->mode)));

    if (context.Button("Follow Active Body##viewport-follow-active"))
    {
        try
        {
            session.Viewports().FollowActiveBody(id);
            owner_->status_ =
                "Controlled viewport now follows the shared active body.";
        }
        catch (const std::exception& exception)
        {
            owner_->status_ = exception.what();
        }
    }

    if (target->target.has_value())
    {
        context.KeyValue("Target", target->target->name);
        context.KeyValue("Body", target->target->body.ToString());
        context.KeyValue("Frame", target->target->frame.ToString());

        const auto bodyObject =
            session.World().Universe().ObjectForBody(
                target->target->body);
        if (bodyObject.has_value())
        {
            const auto proxies =
                world_model::ResolveVisibilityProxies(
                    session.World().Objects(),
                    *bodyObject);
            const auto dynamicCount =
                std::count_if(
                    proxies.begin(),
                    proxies.end(),
                    [](const auto& proxy)
                    {
                        return proxy.dynamic;
                    });
            context.KeyValue(
                "Visibility proxies",
                std::format(
                    "{} authored · {} dynamic",
                    proxies.size(),
                    dynamicCount));
        }
    }
    else
    {
        context.MutedText("Target: none");
    }

    context.Separator();
    context.MutedText("Pin body");

    const auto& universe = session.World().Universe();
    const auto& bodies = universe.Bodies();
    for (const auto systemId : bodies.Systems())
    {
        const auto* system = bodies.FindSystem(systemId);
        if (system != nullptr)
        {
            context.Text(system->name);
        }

        for (const auto bodyId : bodies.Bodies(systemId))
        {
            const auto* body = bodies.FindBody(bodyId);
            const auto semantic = universe.ObjectForBody(bodyId);
            if (body == nullptr || !semantic.has_value())
            {
                continue;
            }

            const bool selected =
                target->target.has_value() &&
                target->target->semanticObject == *semantic;
            std::string label = body->name;
            label += "##viewport-target-body:";
            label += semantic->ToString();

            if (context.Selectable(label, selected))
            {
                try
                {
                    session.Viewports().PinToObject(
                        id,
                        *semantic);
                    owner_->status_ =
                        "Viewport pinned to " + body->name + ".";
                }
                catch (const std::exception& exception)
                {
                    owner_->status_ = exception.what();
                }
            }
        }
    }
}

void StudioExpansionShell::DrawBezierProperties(
    editor_ui::PanelContext& context)
{
    if (!BezierContextRelevant())
    {
        return;
    }

    auto& paths = owner_->session_->PathNetwork().Service();
    for (const auto object :
         owner_->session_->World().Selection().Ordered())
    {
        const auto edge = paths.FindEdge(object);
        if (!edge.has_value() ||
            edge->mode != paths::EdgeMode::Bezier)
        {
            continue;
        }

        math::Double3 startHandle = edge->startHandleMeters;
        math::Double3 endHandle = edge->endHandleMeters;

        bool changed = context.InputDouble3(
            "Start Handle (m)##bezier-start-handle",
            startHandle);
        changed = context.InputDouble3(
            "End Handle (m)##bezier-end-handle",
            endHandle) || changed;

        if (changed)
        {
            try
            {
                paths.SetBezierHandles(
                    edge->id,
                    startHandle,
                    endHandle);
                owner_->status_ =
                    "Bezier handles updated. Undo/redo uses the shared command transaction stack.";
            }
            catch (const std::exception& exception)
            {
                owner_->status_ = exception.what();
            }
        }
        return;
    }
}

void StudioExpansionShell::DrawViewportDiagnosticsProperties(
    editor_ui::PanelContext& context)
{
    if (!ViewportControlsRelevant())
    {
        return;
    }

    const std::string_view id = SelectedViewportId();
    const auto* target = owner_->session_->Viewports().Find(id);

    context.Text(
        id == "studio.map"
            ? "Diagnostics: Body Map / Debug View"
            : "Diagnostics: Primary");

    {
        bool textHud = owner_->views_->TextDiagnosticsHud(id);
        if (context.Checkbox(
                "Text readout (position, heights, biome)##diag-text",
                textHud))
        {
            owner_->views_->SetTextDiagnosticsHud(id, textHud);
        }
    }

    if (target->mode == studio_session::ViewportMode::Perspective)
    {
        if (context.Section("Terrain layers##diag-layers", true))
        {
            auto layers = owner_->views_->TerrainLayers(id);
            bool layersChanged = false;

            layersChanged =
                context.Checkbox(
                    "Near-field terrain##layer-production",
                    layers.productionSurface) || layersChanged;
            layersChanged =
                context.Checkbox(
                    "Full clipmap renderer (ground to orbit, no globe)##layer-full-clipmap",
                    layers.fullClipmap) || layersChanged;
            layersChanged =
                context.Checkbox(
                    "Orbital globe patches##layer-macro",
                    layers.macroGlobe) || layersChanged;
            layersChanged =
                context.Checkbox(
                    "Ocean##layer-ocean",
                    layers.ocean) || layersChanged;
            layersChanged =
                context.Checkbox(
                    "Surface effects##layer-effects",
                    layers.surfaceEffects) || layersChanged;
            layersChanged =
                context.Checkbox(
                    "Clouds##layer-clouds",
                    layers.clouds) || layersChanged;
            layersChanged =
                context.Checkbox(
                    "Cloud light volume##layer-cloud-light-volume",
                    layers.cloudLightVolume) || layersChanged;
            context.MutedText("Bypass a frame stage (for bisecting artefacts):");
            layersChanged =
                context.Checkbox(
                    "Bypass cloud shadow##bypass-cloud-shadow",
                    layers.bypassCloudShadow) || layersChanged;
            layersChanged =
                context.Checkbox(
                    "Bypass proxy sun shadow##bypass-proxy-sun-shadow",
                    layers.bypassProxySunShadow) || layersChanged;
            layersChanged =
                context.Checkbox(
                    "Bypass proxy surfaces (draw proxies as geometry)##bypass-proxy-surfaces",
                    layers.bypassProxySurfaces) || layersChanged;
            layersChanged =
                context.Checkbox(
                    "Bypass mesh surfaces (imported Static Meshes)##bypass-mesh-surfaces",
                    layers.bypassMeshSurfaces) || layersChanged;
            layersChanged =
                context.Checkbox(
                    "Bypass SDF GI (world-space fallback of the final gather)##bypass-sdf-gi",
                    layers.bypassSdfGi) || layersChanged;
            layersChanged =
                context.Checkbox(
                    "Leave terrain out of the SDF##bypass-sdf-terrain",
                    layers.bypassSdfTerrain) || layersChanged;
            layersChanged =
                context.Checkbox(
                    "Leave proxies out of the SDF##bypass-sdf-proxies",
                    layers.bypassSdfProxies) || layersChanged;
            layersChanged =
                context.Checkbox(
                    "Bypass sky cache fill (sky-only radiance cache channel)##bypass-sky-cache",
                    layers.bypassSkyCache) || layersChanged;
            layersChanged =
                context.Checkbox(
                    "Bypass indirect lighting (final gather + reflections)##bypass-indirect",
                    layers.bypassIndirectLighting) || layersChanged;
            layersChanged =
                context.Checkbox(
                    "Bypass hybrid reflections only##bypass-hybrid-reflections",
                    layers.bypassHybridReflections) || layersChanged;
            layersChanged =
                context.Checkbox(
                    "Bypass radiance cache fallback only##bypass-radiance-cache",
                    layers.bypassRadianceCache) || layersChanged;
            layersChanged =
                context.Checkbox(
                    "Bypass near-field water##bypass-near-water",
                    layers.bypassNearFieldWater) || layersChanged;
            layersChanged =
                context.Checkbox(
                    "Bypass atmosphere (and clouds)##bypass-atmosphere",
                    layers.bypassAtmosphere) || layersChanged;
            layersChanged =
                context.Checkbox(
                    "Indirect lighting coverage view##indirect-coverage",
                    layers.indirectCoverageView) || layersChanged;
            layersChanged =
                context.Checkbox(
                    "GI only view (final gather + radiance cascades)##gi-only",
                    layers.giOnlyView) || layersChanged;
            {
                static constexpr std::array<std::string_view, 3>
                    kAntiAliasingModes{"Off", "FXAA", "TAA (FXAA fallback)"};
                i32 antiAliasing = static_cast<i32>(layers.antiAliasing);
                if (context.Combo(
                        "Anti-aliasing##layer-anti-aliasing",
                        kAntiAliasingModes,
                        antiAliasing))
                {
                    layers.antiAliasing =
                        static_cast<u8>(std::clamp(antiAliasing, 0, 2));
                    layersChanged = true;
                }
                f64 giIntensity = static_cast<f64>(layers.giIntensity);
                if (context.SliderDouble(
                        "GI intensity (pi = physical)##layer-gi-intensity",
                        giIntensity,
                        0.0,
                        8.0))
                {
                    layers.giIntensity = static_cast<f32>(giIntensity);
                    layersChanged = true;
                }
                f64 renderScale = static_cast<f64>(layers.renderScale);
                if (context.SliderDouble(
                        "Render scale##layer-render-scale",
                        renderScale,
                        0.25,
                        1.0))
                {
                    layers.renderScale = static_cast<f32>(renderScale);
                    layersChanged = true;
                }
                static constexpr std::array<std::string_view, 6> kSdfModes{
                    "Off", "Shaded", "Steps", "Distance", "Split (SDF | scene)",
                    "Surface radiance"};
                i32 sdfMode = static_cast<i32>(layers.sdfDebugView);
                if (context.Combo(
                        "Mesh SDF debug view##layer-sdf-debug",
                        kSdfModes,
                        sdfMode))
                {
                    layers.sdfDebugView = static_cast<u8>(std::clamp(sdfMode, 0, 5));
                    layersChanged = true;
                }
                f64 jitterScale = static_cast<f64>(layers.taaJitterScale);
                if (context.SliderDouble(
                        "TAA jitter scale##layer-taa-jitter",
                        jitterScale,
                        0.0,
                        1.0))
                {
                    layers.taaJitterScale = static_cast<f32>(jitterScale);
                    layersChanged = true;
                }
                f64 shadowSoftness = static_cast<f64>(layers.meshShadowSoftness);
                if (context.SliderDouble(
                        "Mesh shadow softness (x real sun)##layer-mesh-shadow-softness",
                        shadowSoftness,
                        0.0,
                        8.0))
                {
                    layers.meshShadowSoftness = static_cast<f32>(shadowSoftness);
                    layersChanged = true;
                }
            }
            f64 volumeDebugAltitude = static_cast<f64>(layers.cloudVolumeDebugAltitude);
            if (context.SliderDouble(
                    "Light volume debug slice (m, 0 = off)##layer-cloud-volume-debug",
                    volumeDebugAltitude,
                    0.0,
                    20000.0))
            {
                layers.cloudVolumeDebugAltitude = static_cast<f32>(volumeDebugAltitude);
                layersChanged = true;
            }
            f64 godrayStrength = static_cast<f64>(layers.cloudGodrayStrength);
            if (context.SliderDouble(
                    "God-ray strength##layer-cloud-godrays",
                    godrayStrength,
                    0.0,
                    2.0))
            {
                layers.cloudGodrayStrength = static_cast<f32>(godrayStrength);
                layersChanged = true;
            }

            f64 bias = static_cast<f64>(layers.lodBiasStops);
            if (context.SliderDouble(
                    "LOD bias (stops)##layer-lod-bias",
                    bias,
                    -4.0,
                    4.0))
            {
                layers.lodBiasStops = static_cast<f32>(bias);
                layersChanged = true;
            }
            context.MutedText(
                "+ keeps richer terrain longer and doubles orbital patch detail; - is coarser and cheaper.");

            layersChanged =
                context.Checkbox(
                    "Dynamic clipmap levels##layer-dynamic-clipmaps",
                    layers.dynamicClipmaps) || layersChanged;
            f64 pixelsPerVertex = static_cast<f64>(layers.clipmapPixelsPerVertex);
            if (context.SliderDouble(
                    "Pixels per vertex##layer-clipmap-ppv",
                    pixelsPerVertex,
                    0.25,
                    16.0))
            {
                layers.clipmapPixelsPerVertex = static_cast<f32>(pixelsPerVertex);
                layersChanged = true;
            }
            f64 fadeSeconds = static_cast<f64>(layers.clipmapFadeSeconds);
            if (context.SliderDouble(
                    "Level fade (s)##layer-clipmap-fade",
                    fadeSeconds,
                    0.0,
                    3.0))
            {
                layers.clipmapFadeSeconds = static_cast<f32>(fadeSeconds);
                layersChanged = true;
            }
            layersChanged =
                context.Checkbox(
                    "EXPERIMENT: distance-banded levels##layer-distance-bands",
                    layers.experimentalDistanceBands) || layersChanged;
            f64 bandScale = static_cast<f64>(layers.clipmapBandScale);
            if (context.SliderDouble(
                    "Band distance scale##layer-band-scale",
                    bandScale,
                    0.25,
                    4.0))
            {
                layers.clipmapBandScale = static_cast<f32>(bandScale);
                layersChanged = true;
            }
            if (layers.experimentalDistanceBands)
            {
                // One rendering distance per clipmap level: level k is drawn
                // between the previous level's distance and this one.
                for (std::size_t edgeIndex = 0U;
                     edgeIndex < layers.clipmapBandEdgesMeters.size() &&
                     layers.clipmapBandEdgesMeters[edgeIndex] > 0.0F;
                     ++edgeIndex)
                {
                    f64 edge = static_cast<f64>(layers.clipmapBandEdgesMeters[edgeIndex]);
                    const f64 lower = edgeIndex == 0U
                        ? 1.0
                        : static_cast<f64>(layers.clipmapBandEdgesMeters[edgeIndex - 1U]) * 1.05;
                    const std::string label = std::format(
                        "Level {} reaches (m)##layer-band-edge-{}", edgeIndex, edgeIndex);
                    if (context.InputDouble(label, edge))
                    {
                        // Keep the edges increasing; the next one is pushed out if needed.
                        edge = std::max(edge, lower);
                        layers.clipmapBandEdgesMeters[edgeIndex] = static_cast<f32>(edge);
                        for (std::size_t next = edgeIndex + 1U;
                             next < layers.clipmapBandEdgesMeters.size() &&
                             layers.clipmapBandEdgesMeters[next] > 0.0F;
                             ++next)
                        {
                            layers.clipmapBandEdgesMeters[next] = std::max(
                                layers.clipmapBandEdgesMeters[next],
                                layers.clipmapBandEdgesMeters[next - 1U] * 1.05F);
                        }
                        layersChanged = true;
                    }
                }
            }
            context.MutedText(
                "Draws clipmap level k only where the camera's distance to the terrain is in its band "
                "(default 0-100 m, 100-500 m, 500-2 km, 2-10 km, ...), cross-fading neighbours so the rings "
                "resize with the camera. Needs the full clipmap renderer. Edit the bands over RPC "
                "(clipmap_band_edges_meters).");
            context.MutedText(
                "Dynamic levels draw only the clipmap levels the camera can use: fine levels vanish "
                "as you rise, coarse ones when ground is not in view. Lower pixels-per-vertex keeps finer levels.");

            if (context.Button("Reset layers##layer-reset"))
            {
                layers = {};
                layersChanged = true;
            }

            if (layersChanged)
            {
                owner_->views_->SetTerrainLayers(id, layers);
            }
        }

        auto diagnostics =
            owner_->views_->TerrainDiagnosticOverlays(id);
        bool changed = false;

        const auto toggle =
            [&context, &changed](
                const char* label,
                bool& value)
            {
                changed =
                    context.Checkbox(label, value) || changed;
            };

        toggle(
            "Dirty page bounds##diag-dirty",
            diagnostics.dirtyPageBounds);
        toggle(
            "Build / upload states##diag-build",
            diagnostics.buildStates);
        toggle(
            "Physical LOD##diag-physical-lod",
            diagnostics.physicalLod);
        toggle(
            "Active clipmap rings##diag-clipmap-rings",
            diagnostics.clipmapRings);
        toggle(
            "Tint terrain by clipmap level##diag-clipmap-levels",
            diagnostics.clipmapLevels);
        toggle(
            "Clipmap sample health (bad elevation/morph/slope)##diag-clipmap-sample-health",
            diagnostics.clipmapSampleHealth);
        toggle(
            "Clipmap hole / fade view (why vertices are culled)##diag-clipmap-hole-view",
            diagnostics.clipmapHoleView);
        toggle(
            "Clipmap projected position view (clipped vertices)##diag-clipmap-projection-view",
            diagnostics.clipmapProjectionView);
        toggle(
            "Clipmap shading view (bad interpolated inputs)##diag-clipmap-shading-view",
            diagnostics.clipmapShadingView);
        toggle(
            "Clipmap wireframe##diag-clipmap-wireframe",
            diagnostics.clipmapWireframe);
        toggle(
            "Freeze clipmaps##diag-clipmap-freeze",
            diagnostics.clipmapFreeze);
        toggle(
            "Cache status##diag-cache",
            diagnostics.cacheStatus);
        toggle(
            "Authored constraints##diag-constraints",
            diagnostics.authoredConstraints);
        toggle(
            "Biome weights##diag-biome",
            diagnostics.biomeWeights);
        toggle(
            "Process masks##diag-process",
            diagnostics.processMasks);
        toggle(
            "Drainage vectors##diag-drainage",
            diagnostics.drainageVectors);

        if (changed)
        {
            owner_->views_->SetTerrainDiagnosticOverlays(
                id,
                diagnostics);
        }

        if (diagnostics.cacheStatus)
        {
            const auto runtime =
                owner_->session_->TerrainRuntime().Capture(id);
            if (runtime.has_value())
            {
                const auto* services =
                    owner_->session_->World().Surfaces().ServicesForBody(
                        runtime->body);
                if (services != nullptr)
                {
                    const auto stats = services->Cache().Stats();
                    context.Text(
                        std::format(
                            "M26 cache: {} pages · {} bytes · hits {} · misses {} · evictions {}",
                            stats.residentPages,
                            stats.residentBytes,
                            stats.hits,
                            stats.misses,
                            stats.evictions));
                }

                const auto rebuild =
                    owner_->session_->TerrainPhysicalPages().BodyStatus(
                        runtime->planet.id);
                if (rebuild.has_value())
                {
                    context.Text(
                        std::format(
                            "M06 pages: {} dirty · {} queued · {} building · {} uploading · {} ready",
                            rebuild->dirtyPages,
                            rebuild->queuedPages,
                            rebuild->buildingPages,
                            rebuild->uploadingPages,
                            rebuild->readyPages));
                }
            }
        }
        return;
    }

    if (target->mode != studio_session::ViewportMode::Debug)
    {
        context.MutedText(
            "Terrain diagnostics are available in Perspective or Debug mode.");
        return;
    }

    const auto catalog = terrain_debug::FieldCatalog();
    std::vector<std::string_view> fieldNames;
    fieldNames.reserve(catalog.size());

    i32 selectedFieldIndex = 0;
    const auto selectedField = owner_->views_->DebugField(id);
    for (std::size_t index = 0U; index < catalog.size(); ++index)
    {
        fieldNames.push_back(catalog[index].name);
        if (catalog[index].field == selectedField)
        {
            selectedFieldIndex = static_cast<i32>(index);
        }
    }

    if (!fieldNames.empty() &&
        context.Combo(
            "Field##viewport-debug-field",
            fieldNames,
            selectedFieldIndex))
    {
        selectedFieldIndex = std::clamp<i32>(
            selectedFieldIndex,
            0,
            static_cast<i32>(catalog.size()) - 1);
        owner_->views_->SetDebugField(
            id,
            catalog[static_cast<std::size_t>(selectedFieldIndex)].field);
    }

    i64 pageLevel = static_cast<i64>(
        owner_->views_->DebugPhysicalPageLevel(id));
    if (context.InputInteger(
            "Physical page tile level##viewport-debug-page-level",
            pageLevel))
    {
        pageLevel = std::clamp<i64>(pageLevel, 0, 30);
        owner_->views_->SetDebugPhysicalPageLevel(
            id,
            static_cast<u8>(pageLevel));
        owner_->status_ =
            "Physical page level changed; the center page will be reselected.";
    }

    const auto selectedPage = owner_->views_->DebugPhysicalPage(id);
    if (selectedPage.has_value())
    {
        const auto& tile = selectedPage->address.tile;
        context.Text(
            std::format(
                "Physical page: {} L{} ({}, {})",
                CubeFaceName(tile.face),
                tile.level,
                tile.x,
                tile.y));
    }
    else
    {
        context.MutedText(
            "Physical page: none · click terrain in the debug viewport.");
    }

    const auto livePage = owner_->views_->LiveDebugPage(id);
    if (livePage == nullptr)
    {
        context.MutedText(
            "Live products: no physical-page snapshot published yet.");
        return;
    }

    const auto field = owner_->views_->DebugField(id);
    context.Text(
        std::format(
            "Live products: ready · physical LOD {} · {}x{}",
            livePage->Stamp().physicalLod,
            livePage->Width(),
            livePage->Height()));
    context.Text(
        livePage->Has(field)
            ? "Selected field: available"
            : "Selected field: not published by the live page producer");

    if (!livePage->Has(field))
    {
        return;
    }

    const auto seams =
        terrain_debug::InspectTerrainDebugSeams(
            *livePage,
            field,
            owner_->session_->TerrainDebugPages());

    context.MutedText("Physical page seams");
    for (const auto& seam : seams)
    {
        if (seam.state ==
            terrain_debug::TerrainDebugSeamState::ValueMismatch)
        {
            context.Text(
                std::format(
                    "{}: {} ({}/{} samples, max diff {:.6g})",
                    EdgeName(seam.edge),
                    terrain_debug::TerrainDebugSeamStateName(seam.state),
                    seam.comparison.mismatchedSamples,
                    seam.comparison.samplesCompared,
                    seam.comparison.maximumDifference));
        }
        else
        {
            context.Text(
                std::format(
                    "{}: {}",
                    EdgeName(seam.edge),
                    terrain_debug::TerrainDebugSeamStateName(seam.state)));
        }
    }

    const auto& descriptor = terrain_debug::Descriptor(field);
    context.MutedText("Upstream provenance");
    for (const auto stage : descriptor.upstream)
    {
        context.Text(
            std::format(
                "- {}",
                terrain_debug::StageName(stage)));
    }
}
} // namespace orbit::studio_ui

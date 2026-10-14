+++
path = "/world/roads-railways/editor-builder"
title = "Procedural Paths and Roads - Cities-style Studio construction menu"
kind = "concept"
status = "experimental"
summary = "Build > Procedurals > Procedural Paths & Roads menu, palette, interaction model, viewport click-to-build, snapping and honest construction limitations."
keywords = ["road", "procedural", "city builder", "editor", "menu", "viewport", "snap", "rail", "ui"]
related = ["/world/roads-railways", "/world/roads-railways/pipeline", "/world/roads-railways/spec-completeness"]
sources = ["apps/editor/src/StudioProceduralsPanel.hpp", "apps/editor/src/StudioViewportPanel.cpp", "apps/editor/src/StudioPanels.hpp", "apps/editor/src/StudioApplication.cpp", "engine/editor_ui/src/EditorUi.cpp"]
verify = ["Manual: Build > Procedurals > Procedural Paths & Roads", "Manual: draw, snap, undo, reopen and inspect native road geometry", "python tools/orbit_docs_cli.py check"]
+++

# Discoverability and navigation

**Main menu:** Build → Procedurals → Procedural Paths & Roads. This is a real nested Build menu in the canonical EditorUi action list, not a renamed flat entry or a second editor. Build → Procedurals → All Procedural Tools opens the Procedurals hub.

The existing **Procedurals** dock panel remains the authority for this tool. Its landing page offers Procedural Paths & Roads and Advanced Infrastructure Editor. Selecting Roads opens a focused, city-builder-inspired palette with a type selector, cross-section preview, alignment modes, construction levels, speed, curve easing, endpoint snap radius, network selection and one prominent Start/Finish drawing action. Advanced details and engineering/debug tools are still one click away. The panel stays dockable and respects Studio layout persistence.

# Supported catalog vs future section types

| Palette type | Current authored implementation | Honest restriction |
| --- | --- | --- |
| Five paved road presets: service 3.5 m/1 lane, urban street 6.5 m/2 lanes, rural 7 m/2 lanes, avenue 14 m/4 lanes, expressway 21 m/6 lanes | Shared RoadCatalog + PathProfile → PlanInfrastructureSection → native corridor mesh, lanes and contact | Cross-section recipe works; markings, curbs, multilane traffic and special junction geometry remain incomplete |
| Single ballasted railway track | ballasted-rail native corridor catalog | True turnout geometry and track train dynamics incomplete |
| Reference path | General authored PathNetwork edges without infrastructure catalog | Not a physical drivable road |
| Ground | constructionMode=terrain | Native terrain construction supported slice |
| Bridge / Tunnel / Deck | Existing construction intent on the edge | Full support assemblies, tunnel excavation and collision NOT implemented |

The five named paved widths are real geometric profiles, not visual-only placeholders. Custom street furniture, traffic regulation, sidewalks, central medians, highway ramps, and arbitrary custom sections are planned and must not be presented as finished.

# Click-to-build behavior

1. User selects category/type, straight/curve/terrain-route, supported ground or explicit structural intent, and optionally an existing network.
2. Start Drawing activates the **existing viewport Path Tool** plus a presentation-only infrastructure mode. The viewport remains the camera/selection owner.
3. First click projects onto the active celestial body's reference surface and authors a body-fixed FramePoint node. In this mode, unlike the original surface-anchored generic Path Tool, frame-local anchors are used so the native corridor construction cache can consume the result. This represents the reference ellipsoid; **actual terrain height conforming and cut/fill acceptance must still be verified**.
4. Each following click creates another authored node (or reuses a valid nearby node in the same network and body-fixed frame). In the same undoable command transaction, it connects to the previous node through PathNetworkService ConnectDirect, ConnectBezier or ConnectRouted and persists SetInfrastructureIntent for a selected road/rail catalog. Generic reference paths are connected without imposing road/rail intent.
5. Click again to extend a chain; Finish exits construction and clears the previous point. The separate ordinary Path Tool continues to place nodes/select pairs as before.
6. Endpoint snapping is explicit, default 10 m (editable 1–50 m). It only matches an existing authored **node** in the same network/frame; never geometrically overlays an unrelated road or auto-splits a mid-edge intersection. The source network ID is preserved.

Bezier construction handles use the body-local segment delta with adjustable lateral bias, with a clean rollback for unsupported geometry. Routed placement requires the existing routing backend. Users can edit authored path geometry and parameters via the advanced Procedurals editor, preserving all previous capability.

# Engine contracts and verification

- No duplicate road authority or renderer: network writes go through PathNetworkService/CommandService; rendering remains the existing path cache and unified Vulkan Paths pass.
- Transaction includes node + connected edge + construction intent. Exceptions roll it back; only current successful creations update the tool's last-node chain.
- The palette controls are presentation/tool state, not persisted world state. The authored network and infrastructure intent **are** persisted through existing commands.
- New editor nested-menu functionality groups successive MenuAction labels of the form "Parent / Child"; other ordinary top-level items remain unchanged.
- Manual smoke gate: open a body/world, use Build → Procedurals → Procedural Paths & Roads, create a two- or three-segment road; confirm it appears in the existing Procedurals list, inspect source metadata, undo one click transaction, reconnect to an existing node via snapping, edit alignment, save/reopen world, and verify resulting road construction/error status.
- Manual regression gate: toggle original viewport Path Tool outside palette and confirm it continues placing ordinary nodes, not automatically creating roads. Switch road/rail/reference categories and verify native type and stored catalog ID. Test a bridge selection reports intent-only, not claimed physical geometry.
- Native Windows compile/UI smoke and screenshot/profiler results are not verified by source edits alone.

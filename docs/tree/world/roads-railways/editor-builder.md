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
3. The first click projects onto the active celestial body's reference surface, samples the composed terrain elevation and authors a body-fixed FramePoint start node. The point uses terrain elevation plus the editor offset; the full road corridor still needs its own grading and terrain-conformance validation.
4. **Straight/Routed:** each following click creates or reuses an endpoint and constructs a segment as one undoable transaction. **Curve:** the second click sets the *outgoing tangent direction* and its handle length in the start node's local tangent plane; it does **not** create an object or undo entry. The third click creates or snaps the end node and commits a cubic Bezier edge with its infrastructure intent, as one undoable transaction.
5. In Curve mode the completed endpoint becomes the next start, so extending a road takes two clicks per subsequent segment: direction, then endpoint. Right-click or Enter ends the current chain without disarming construction; Escape exits the tool. The ordinary generic Path Tool remains separate. The separate ordinary Path Tool continues to place nodes/select pairs as before.
6. Endpoint snapping is explicit, default 10 m (editable 1–50 m). It only matches an existing authored **node** in the same network/frame; never geometrically overlays an unrelated road or auto-splits a mid-edge intersection. The source network ID is preserved.

Curve mode derives the first Bezier handle from the second click's tangent-plane direction and distance, clamped to half the completed endpoint chord. The end handle follows one third of the negative chord by default. Live preview and committed geometry share this same calculation; the expert editor can subsequently modify both handles. Routed placement requires the existing routing backend. Users can edit authored path geometry and parameters via the advanced Procedurals editor, preserving all previous capability.

# Engine contracts and verification

- No duplicate road authority or renderer: network writes go through PathNetworkService/CommandService; rendering remains the existing path cache and unified Vulkan Paths pass.
- Transaction includes node + connected edge + construction intent. Exceptions roll it back; only current successful creations update the tool's last-node chain.
- The palette controls are presentation/tool state, not persisted world state. The authored network and infrastructure intent **are** persisted through existing commands.
- New editor nested-menu functionality groups successive MenuAction labels of the form "Parent / Child"; other ordinary top-level items remain unchanged.
- Manual smoke gate: open a body/world, use Build → Procedurals → Procedural Paths & Roads, create a two- or three-segment road; confirm it appears in the existing Procedurals list, inspect source metadata, undo one click transaction, reconnect to an existing node via snapping, edit alignment, save/reopen world, and verify resulting road construction/error status.
- Manual regression gate: toggle original viewport Path Tool outside palette and confirm it continues placing ordinary nodes, not automatically creating roads. Switch road/rail/reference categories and verify native type and stored catalog ID. Test a bridge selection reports intent-only, not claimed physical geometry.
- Native Windows compile/UI smoke and screenshot/profiler results are not verified by source edits alone.

## Builder usability and end-to-end authoring contract

The construction page now starts with a prominent Start/Finish button, followed by real catalog sections, an accurate lane-count and width schematic, alignment and snap controls, and numeric placement elevation. Network editing, engineering, road upgrades and construction diagnostics are progressive-disclosure sections. Expert path editing is one click away, with no second graph authority.

**Viewport preview:** while armed, hovering projects a body-reference ray and renders a ghost chord or cubic Bezier with two pavement-boundary guides using the currently selected catalog's actual width. For Curve mode, before the direction click it displays a guide from start to cursor, *not* a prematurely committed road; after the direction click, moving the cursor previews the final curve before the third-click commit. Railway preview uses its formation width. Routed edges explicitly show a dashed chord only because a computed terrain route can differ. A ring indicates a valid snapped node. The status reports mode, length, snap and the last created edge ID.

**Snapping and safety:** the preview and click share a nearest authored endpoint lookup restricted to the selected network and body-fixed frame. Its presentation cache is keyed by object-store revision, network and frame to avoid a fresh full scan on every pointer movement. Clicking the same endpoint rolls back the empty transaction; segments shorter than 0.5 m reject before committing. A successful click writes new node, edge and catalog intent using the preexisting CommandService transaction. No automatic crossing/intersection is inferred from visual proximity.

**Placement controls:** Curve uses start → direction → endpoint, then direction → endpoint for each extension. Straight and Routed use start → endpoint. Right click or Enter finishes the current chain without exiting the construction mode; Escape exits. Page Up / Page Down adjust placement elevation by 1 m, or 5 m with Shift; numeric entry and Raise/Lower/Reset controls use the same state. Values are clamped between -10,000 m and +100,000 m. Height is added to the composed terrain sample at each authored road node; this does not imply full road-profile grading or actual bridge supports. Switching alignment resets the in-progress chain and direction; switching network or road/rail/path family resets it. World close resets all editor-only preview caches, counters and errors.

**Road section upgrade:** choose an already authored catalog-backed road, choose a replacement real catalog profile, and run Upgrade. The existing authored edge ID, anchors, alignment and construction level are preserved; profile width/lane count and design-speed/easing intent change via the existing undoable SetInfrastructureIntent command. Corridor, junction and engineering products rebuild through their normal pipeline. This is not a full multi-lane traffic/intersection regulation system.

**Built-in diagnostics:** expandable inventory shows authored networks/nodes/edges, number of cached native corridors, dirty pending builds and errors. This does not declare tunnels/bridges, routed physical corridors, NPC traffic or drivable vehicle physics complete.

### Native acceptance checklist (not yet executed)

1. Open Build → Procedurals → Procedural Paths & Roads; select five paved road profiles and verify diagram width/lanes.
2. Start construction, hover to see accurate ghost boundaries, place three connected segments and verify undo of the last node+edge+intent as one transaction.
3. Snap an endpoint; click the same node again and verify no extra edge or undo history. Right-click to end a chain, place a second chain, Escape to exit; verify the original Path Tool remains node-only.
4. Use Page Up/Down and exact height: verify frame-local authored node positions change by the intended body-normal offset and survive world save/reopen.
5. Upgrade a two-lane edge to the four- or six-lane catalog, inspect its native mesh and lane references, and Undo to recover the old section.
6. Change world and verify transient draw state and snap cache do not leak. Try bridge/tunnel/routed modes; keep their incomplete physical construction explicitly labelled pending.

Source-level changes alone are not a passing Windows runtime, Vulkan capture or Cities: Skylines feature-completeness test.

## Bulk road management workflow

The Roads palette now has **Road manager / bulk upgrade**, a bounded scrollable list showing authored road name, native preset, lane count and one of READY / BUILDING / NOT BUILT / ERROR from the existing generation cache. Case-insensitive search matches road name, catalog ID or catalog label. "Only selected network" and "Show build errors only" narrow the list. "Select visible" and "Clear selection" keep multi-selection explicit. Selecting a row also defines the road eligible for detailed inspection.

**Upgrade selected roads (undoable)** changes selected eligible road profiles to the currently selected palette preset, design speed and curve-easing setting. It uses one CommandService transaction for all edges; any thrown write rolls the whole transaction back. For every edge, original alignment, endpoint/node identities and terrain/bridge/tunnel/deck construction intent remain unchanged. Edges already using those exact parameters are skipped. Editing a new world prunes missing ID selections rather than sending stale IDs to the service. The operation changes authored catalog intents; async native geometry and contact publication follow the usual rebuild budget.

This does not claim retroactive roadway lane routing, traffic rerouting or an automated Cities: Skylines road-lane junction designer. Native Windows UI smoke must verify multi-selection with mixed sections, upgrade, Undo, Redo, edited cache rebuild and saving/reopening.

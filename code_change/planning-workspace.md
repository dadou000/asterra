# Planning workspace placement

- Existing owner: `StudioExpansionShell` and `StudioViewportPanels` own the
  workspace shell, mode switching, and contextual tool bands.
- Primary insertion points: a dedicated shell-band row for workspace tabs;
  `StudioPlanningUi` and its project-local plan store for the planning canvas;
  the existing RPC dispatcher and MCP adapter for automation parity.
- Canonical state: project `Planning/implementation-plan.json` stores bubble
  titles, descriptions, statuses, positions, and optional predecessor links.
  The canvas is only a view over that store.
- Do not duplicate mode state, report storage, or plan mutations in UI-only
  callbacks. UI and RPC call the same store operations.
- Navigation polish belongs in the existing EditorUi toolkit: one tab control
  draws consistent vector icons and selected/hover/focus states. Studio supplies
  labels and forwards selection to its existing workspace operation.
- Build keeps its transform selector in the tool band. The context band only
  supplies it for other viewport workspaces, using the same gizmo state.
- Toolbar polish extends PanelContext with vector icon buttons, exclusive icon
  choices, and a drawn group divider. Studio retains all action callbacks and
  canonical gizmo/command state; selected and disabled styles are presentation.
  Native edits use the existing central automatic Studio generation handoff.
- Creation categories reuse StudioExpansionShell's command catalog and existing
  argument form; menu actions invoke the current creation/command owners.
  Remove the breadcrumb band reservation, but draw command popups from the
  remaining context band so shortcuts keep working. Compact modifier styling
  stays inside EditorUi and does not create another gizmo state.
- Snapping remains in the existing GizmoSettings/StudioViewportPanels owner.
  Always-visible Snap opens one shared editor for distance/unit, surface, angle,
  and scale steps; the Inspector reuses it. The manipulator still consumes
  meters. RPC/MCP update the same validated settings; units persist with the
  existing Studio shell state. Native saves follow central generation handoff.

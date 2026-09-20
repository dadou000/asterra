# Orbit UI Design Rules

Status: **Canonical engine-wide UI contract**

This document defines the user-interface rules for Orbit Studio, runtime authoring tools, developer tools, plugins, and future agent-facing editor surfaces.

The central rule is:

> **Orbit must be simple and intuitive by default without becoming restrictive. Simplicity may hide complexity, but it must never remove capability.**

These rules are architectural constraints for UI work, not visual-style suggestions.

## 1. Simple first, complete underneath

The first visible interaction should expose the smallest useful set of controls for the current task.

Advanced capability remains available through deeper layers such as:

- Inspector sections;
- expandable Advanced sections;
- contextual toolbars;
- right-click/context menus;
- radial menus;
- command palette/search;
- keyboard shortcuts;
- scripting/automation;
- plugins;
- MCP/agent tooling;
- direct numeric/property editing.

Do not permanently remove or hard-code away a capability merely to keep a panel visually simple.

## 2. Progressive disclosure, not feature reduction

Complexity should appear when it becomes relevant.

A beginner should be able to perform common actions without understanding every engine subsystem.

An expert must still be able to reach the exact low-level controls needed to author, inspect, debug, or override the system.

If a feature has both common and expert parameters, expose a curated ordinary set first and preserve full access in an Advanced surface.

## 3. Contextual UI over permanent clutter

Prefer controls that appear because of selection, mode, viewport state, or current task.

Examples:

- selecting terrain reveals terrain tools;
- selecting a spline reveals spline/path tools;
- selecting a material reveals material controls;
- selecting a vehicle subsystem reveals the relevant mechanical/electrical controls.

Avoid filling the permanent interface with controls unrelated to the current context.

## 4. No artificial workflow gates

Orbit must not force users through wizards, modal sequences, arbitrary modes, or fixed workflows when the underlying operation can safely be performed directly.

Guidance is allowed. Restriction is not.

A wizard may accelerate a common workflow, but the same result must remain reachable through direct authoring.

## 5. Fast path and precise path

Important operations should support both:

- a fast visual interaction;
- a precise numerical or structural interaction.

Examples:

- drag a gizmo, or type the transform;
- paint terrain, or edit the authored mask parameters;
- draw a spline, or edit control points numerically;
- drag a material onto an object, or assign the material through the Inspector.

The visual shortcut must not replace the precise path.

## 6. Direct manipulation should be the default where practical

For spatial and visual tasks, prefer manipulating the thing itself in the viewport instead of forcing the user through detached forms.

Use handles, gizmos, brushes, nodes, splines, drag-and-drop, snapping, and contextual overlays where they improve clarity.

The Inspector remains the authoritative precise editing surface for properties that need exact values.

## 7. The Inspector is never a dead end

Every meaningful selected object should expose its important editable state in a consistent Inspector.

Properties should be searchable and logically grouped.

Advanced or uncommon fields may be collapsed, but must remain accessible when they are legitimate authored controls.

## 8. Search beats memorization

As Orbit grows, users should not need to remember where every tool lives.

Commands, tools, objects, services, materials, assets, settings, and plugin actions should be discoverable through search where practical.

A command palette should expose actions independently of toolbar layout.

## 9. Prefer reversible actions

Authoring actions should use the command/transaction system and support undo/redo whenever the operation is meaningfully reversible.

The UI should encourage experimentation without making users fear irreversible changes.

Destructive operations require clear intent, but confirmation dialogs should not be added to harmless or trivially reversible actions.

## 10. Defaults accelerate; they do not constrain

Orbit should provide strong defaults for common workflows.

Defaults must remain editable.

Templates, presets, automatic setup, and inferred values are accelerators, not locked policy.

A user who understands the system must be able to override defaults unless doing so would violate a genuine engine invariant or corrupt data.

## 11. Distinguish engine invariants from UI restrictions

Some operations are genuinely invalid because they would break an architectural, physical, serialization, or safety invariant.

The UI may prevent those invalid states.

It must not confuse "this is unusual" with "this is forbidden."

When rejecting an action, explain the real invariant and, where possible, provide the valid alternative.

## 12. Do not duplicate authority for UI convenience

The UI is a client of Orbit's real engine/editor services.

It must not create a simplified parallel world representation, terrain representation, asset state, physics state, or other hidden authority just because that is easier to display.

UI state may cache presentation data, but authored state remains owned by the proper engine system.

## 13. One concept, multiple entry points

A capability can be exposed through several interfaces without creating several implementations.

For example, the same command may be invoked from:

- a toolbar button;
- a context menu;
- a radial menu;
- the command palette;
- a keyboard shortcut;
- a plugin;
- an MCP agent.

All entry points should converge on the same underlying command/service.

## 14. Expert speed matters

Frequent users must be able to work quickly.

Where appropriate, support:

- keyboard shortcuts;
- multi-selection;
- batch editing;
- drag-and-drop;
- duplicate/repeat-last-action;
- snapping;
- copy/paste of properties or components;
- numeric expressions;
- command search;
- customizable panels/toolbars;
- macros, plugins, scripting, and MCP automation.

Do not optimize solely for first-time discoverability at the cost of expert efficiency.

## 15. Preserve user control

Automatic systems should expose what they changed and allow the user to override or disable automation when technically valid.

Avoid silent "smart" behavior that rewrites authored data without a visible reason.

Automation should generally propose, preview, derive, or execute through normal commands rather than secretly changing authority.

## 16. Keep feedback local and immediate

When an action is performed, show its result near the place where the user acted whenever practical.

Examples include:

- viewport previews;
- inline validation;
- live status;
- local progress indicators;
- highlighted affected objects or regions;
- clear dirty/building/ready/error states.

Do not force users to search another panel to learn whether an ordinary action worked.

## 17. Errors should be actionable

Error messages should state:

1. what failed;
2. the relevant object/system;
3. why it failed when known;
4. what the user can do next.

Avoid generic "invalid operation" messages when a concrete reason is available.

Developer/debug details may be expandable rather than occupying the main UI.

## 18. Avoid modal interruption

Prefer non-modal panels, inline editing, overlays, and reversible operations.

Use modal dialogs only when the user must make a decision before the application can safely continue.

Do not make routine editing dependent on sequences of blocking dialogs.

## 19. UI layout is customizable

Panels and viewports should support docking, resizing, hiding, restoring, and sensible workspace persistence.

Orbit should provide a strong default layout without treating that layout as mandatory.

Plugins may add panels, commands, inspectors, overlays, tools, and contextual actions through documented extension points.

## 20. Capability must remain accessible without visual clutter

A feature does not need a permanent button to remain a first-class feature.

Before removing a control from the primary UI, verify that the capability remains clearly reachable through at least one appropriate discoverable path.

Rare features may live deeper in the interface.

Essential features must remain easy to find.

## 21. Consistency outranks novelty

The same interaction should behave similarly across subsystems.

Reuse established patterns for:

- selection;
- focus;
- transform manipulation;
- context menus;
- Inspector groups;
- search;
- drag-and-drop;
- undo/redo;
- status and validation;
- destructive actions.

A new custom interaction needs a concrete usability reason.

## 22. Scale from mouse user to automation

Any major authoring capability should be designed so it can eventually be driven by both humans and tooling.

The preferred path is:

```text
UI / shortcuts / plugins / MCP
              |
              v
     command + service APIs
              |
              v
       authoritative state
```

Do not make critical editor features exist only as mouse-generated side effects that cannot be reproduced through engine commands.

## 23. Accessibility and readability are functional requirements

Do not encode essential state by color alone.

Maintain readable contrast, usable hit targets, sensible text sizing, keyboard navigation where practical, and clear focus/selection state.

Dense expert interfaces may exist, but density must come from useful information rather than visual noise.

## 24. Performance is part of usability

Opening a panel, selecting an object, searching commands, dragging controls, navigating the viewport, and editing properties should remain responsive even when expensive engine work is running.

Long operations belong on async/job/GPU paths with explicit progress/state.

Do not make the UI thread perform heavy simulation, generation, compilation, asset processing, or terrain work.

## 25. No arbitrary limits in the UI

Do not impose small fixed limits such as maximum object counts, path nodes, materials, selections, layers, or plugin actions solely because the current widget implementation is easier that way.

If the engine has a real technical limit, expose that limit accurately.

If large data needs virtualization, paging, streaming, LOD, search, or filtering, implement those rather than reducing authoring capability.

## 26. The common-case test

Before accepting a UI feature, verify that a new user can identify the common action with minimal explanation.

If not, simplify naming, grouping, context, affordances, or defaults before adding more documentation.

## 27. The expert-control test

Before accepting a UI simplification, verify that an expert can still access the full legitimate capability of the underlying system.

If simplification removes precision, range, composition, automation, batch operation, inspection, or override capability, the design fails this rule.

## 28. The non-restriction rule

When choosing between:

- removing a capability to simplify the UI; or
- keeping the capability and finding a cleaner way to expose it;

Orbit chooses the second option.

The exception is a capability that violates an explicit engine invariant. Such restrictions belong to engine validation, not arbitrary UI policy.

## 29. One unified Studio application — never separate launchers

Orbit must not split the authoring experience across separate launcher applications, project browsers, setup tools, configuration executables, or disconnected editor shells.

There is one primary Orbit Studio UI, comparable in philosophy to Roblox Studio: startup, project selection, project creation, world editing, play/test, build/export, plugin management, engine/project settings, diagnostics, account/platform integration, and developer tools all belong to the same coherent application surface.

A startup or project-browser view may exist, but it is a state/view of Orbit Studio, not a separate launcher executable or separately designed application.

Rules:

- do not create an `OrbitLauncher`, separate project-manager executable, or equivalent standalone launcher;
- do not require users to close one Orbit application and open another to move from project selection into editing;
- do not duplicate settings, authentication, plugin management, project configuration, build controls, or update controls across multiple shells;
- project browser, recent projects, templates, creation/import, recovery, and workspace entry live inside Orbit Studio;
- editor, runtime-authoring, build/test, profiling, debugging, materials/assets, plugins, and project administration remain navigable inside the same UI framework;
- mode/view transitions may substantially rearrange the workspace, but they remain part of one application and preserve shared state;
- helper/background processes are allowed for technical isolation, compilation, crash handling, asset work, or services, but they must not become separate user-facing launchers or alternative front ends;
- a platform-specific bootstrapper may exist only when technically unavoidable for installation/update mechanics; it must not become the normal project-selection or authoring experience.

The rule is **one product surface, multiple views**, not multiple launchers.

---

# UI review checklist

A new or changed editor surface should be reviewed against these questions:

- Is the common path obvious?
- Is the initial visible control set small enough?
- Can advanced users reach the underlying capability?
- Is exact numeric/structural editing available where appropriate?
- Does the feature use the authoritative command/service path?
- Is the operation undoable when reasonably possible?
- Is there unnecessary modal interaction?
- Is relevant feedback immediate and local?
- Are errors actionable?
- Does automation remain visible and overridable where valid?
- Can the tool be reached through search/command infrastructure when appropriate?
- Can plugins or MCP invoke the same operation without UI-specific duplication?
- Does the UI introduce an arbitrary limit not present in the engine?
- Does the UI remain responsive while expensive work runs?
- Does the design preserve user freedom while enforcing genuine engine invariants?
- Is the feature integrated into the unified Orbit Studio surface rather than creating or depending on a separate launcher/application shell?

A feature that fails the common-case test should be simplified.

A feature that fails the expert-control or non-restriction test should be redesigned rather than stripped down.

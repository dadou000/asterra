+++
path = "/authoring/selection"
title = "Selection service"
kind = "subsystem"
status = "stable"
summary = "SelectionService keeps the ordered set of selected semantic ObjectIds and a revision that changes only when the selection really changes; it depends only on the scene, never on a panel."
owner_module = "OrbitSelection"
keywords = ["selection", "selected", "select", "multi select", "toggle", "selection revision"]
sources = [
  "engine/selection/include/orbit/selection/SelectionService.hpp",
  "engine/selection/CMakeLists.txt",
]
symbols = ["SelectionService"]
invariants = [
  "The selection is an ordered, duplicate-free list of ObjectIds: Set drops null IDs and repeated IDs, preserving first-seen order.",
  "Revision() advances only on a real change: Set with an identical result and Clear on an empty selection leave it untouched; Toggle always changes the selection and so always advances it.",
  "Toggle of a null ObjectId is ignored.",
  "Selection state is session state, not world authority: nothing is persisted and the module depends only on Core and Scene, so panels, plugins and RPC share one selection.",
]
related = ["/authoring/scene", "/editor/session", "/legacy/v0-0-3-spec/12-command-transaction-and-selection-model"]
depends_on = ["/authoring/scene", "/foundation/core"]
used_by = ["/apps/studio", "/authoring/plugins", "/editor/model", "/editor/session"]
verify = [
  "ctest -R Orbit.AuthoringModel",
]
verified = "b0a0de7f"
+++

Consumers watch Revision() to refresh their views (for example ActiveBodyModel follows the selection, `/editor/session`).

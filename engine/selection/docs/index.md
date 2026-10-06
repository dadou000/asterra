+++
path = "/authoring/selection"
title = "Selection service"
kind = "reference"
status = "stable"
summary = "SelectionService keeps the editor's selected semantic objects, independent of any panel."
owner_module = "OrbitSelection"
keywords = ["selection", "selected", "select", "multi select"]
sources = [
  "engine/selection/include/orbit/selection/SelectionService.hpp",
  "engine/selection/CMakeLists.txt",
]
symbols = ["SelectionService"]
related = ["/authoring/scene", "/legacy/v0-0-3-spec/12-command-transaction-and-selection-model"]
depends_on = ["/authoring/scene", "/foundation/core"]
used_by = ["/apps/studio", "/authoring/plugins", "/editor/model", "/editor/session"]
verify = [
  "ctest -R Orbit.AuthoringModel",
]
verified = "b0a0de7f"
+++



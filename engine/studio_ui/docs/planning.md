+++
path = "/editor/studio-ui/planning"
title = "Implementation planning workspace"
kind = "subsystem"
status = "stable"
summary = "A project-local implementation plan presented as movable idea bubbles. PlanningUi edits one ImplementationPlan store; planning.* RPC methods and MCP tools call the same create, update, schedule and delete operations."
owner_module = "OrbitStudioUi"
keywords = ["implementation plan", "planning workspace", "idea bubbles", "schedule after", "planning.list", "planning.update"]
sources = [
  "engine/studio_ui/include/orbit/studio_ui/ImplementationPlan.hpp",
  "engine/studio_ui/include/orbit/studio_ui/PlanningUi.hpp",
  "engine/studio_ui/src/ImplementationPlan.cpp",
  "engine/studio_ui/src/PlanningUi.cpp",
  "tools/mcp_server/orbit_editor_mcp_server.py",
  "docs/ORBIT_MCP.md",
]
symbols = ["ImplementationPlan", "PlanBubble", "PlanBubblePatch", "PlanningUi"]
invariants = [
  "The project file Planning/implementation-plan.json is canonical. Canvas positions, statuses and predecessor links are never kept only in UI state.",
  "A bubble with no predecessor is a floating idea. A predecessor edge schedules it after that bubble; cycles and missing predecessors are rejected.",
  "Deleting a predecessor releases its successors to floating ideas instead of deleting user work.",
  "Canvas drag, detail controls and planning.* RPC/MCP use the same ImplementationPlan mutations and persist after every change.",
  "Workspace tabs map Build to the legacy Scene workspace and Universe to Celestial; old persisted values and RPC aliases remain readable.",
]
related = ["/editor/studio-ui", "/editor/mcp-rpc", "/rules/ui"]
depends_on = ["/editor/ui-toolkit", "/foundation/rpc"]
verify = [
  "Open Planning workspace, create two bubbles, move them, schedule one after the other, restart Studio, and confirm the graph remains.",
  "Over MCP: planning.list, planning.create, planning.update, planning.delete.",
]
verified = ""
+++

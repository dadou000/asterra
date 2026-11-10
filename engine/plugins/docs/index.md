+++
path = "/authoring/plugins"
title = "Plugins (Luau panels and validators)"
kind = "subsystem"
status = "stable"
summary = "PluginManager loads project plugins from <project>/Plugins/<id>/ into per-plugin Luau VMs: restricted panels (PanelContext bridge), validators that reject a project with one diagnostic string, hot reload by source content, and manifest permissions and dependencies."
owner_module = "OrbitPluginManifest"
keywords = ["plugin", "luau", "lua", "panel", "validator", "manifest", "permissions", "hot reload", "scope"]
sources = [
  "engine/plugins/include/orbit/plugins/PluginManager.hpp",
  "engine/plugins/include/orbit/plugins/PluginManifest.hpp",
  "engine/plugins/CMakeLists.txt"]
symbols = ["PluginPanelDescriptor", "PluginPermissionSet"]
invariants = [
  "ProjectManifest::plugins is the authoritative enabled-plugin list for the project; packages live under <project>/Plugins/<id>/.",
  "Plugin hot reload is source-content based (no timestamp resolution assumptions) and returns the number of plugins whose VM was reloaded.",
  "A validator returns nil when valid or one diagnostic string when it rejects the current project.",
  "Plugin panels draw through a restricted PanelContext bridge; a panel that no longer exists makes the draw return false."]
related = ["/authoring/commands", "/rules/hot-iteration"]
depends_on = ["/authoring/commands", "/authoring/documents", "/authoring/scene", "/authoring/schema", "/authoring/selection", "/editor/model", "/editor/ui-toolkit", "/foundation/core"]
used_by = ["/apps/studio", "/editor/session"]
verify = [
  "ctest -R Orbit.Plugins"]
verified = "b0a0de7f"
+++



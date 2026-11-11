+++
path = "/editor/studio-ui/html-panels"
title = "HTML panels (WebView2) with an RPC bridge"
kind = "subsystem"
status = "experimental"
summary = "Studio panels that host an HTML page in Microsoft WebView2. The page calls window.orbit.rpc(method, params) and reaches the same JSON-RPC surface as the MCP tools; saving a file under a web/ folder reloads the panel in place. The Architecture canvas in the Planning workspace is the first page."
owner_module = "OrbitStudioUi"
keywords = ["html panel", "webview2", "web view", "architecture canvas", "orbit.rpc", "html.panel_open", "web asset", "infinite canvas"]
sources = [
  "engine/web_view/include/orbit/web_view/WebBridge.hpp",
  "engine/web_view/src/WebBridge.cpp",
  "engine/web_view/include/orbit/web_view/WebViewHost.hpp",
  "engine/web_view/src/WebViewHost.cpp",
  "engine/studio_ui/include/orbit/studio_ui/HtmlPanelsUi.hpp",
  "engine/studio_ui/src/HtmlPanelsUi.cpp",
  "engine/hot_reload/src/ChangeClassifier.cpp",
  "apps/editor/src/StudioApplication.cpp",
  "apps/editor/web/architecture_canvas.html",
  "apps/editor/web/arch/common.js",
  "apps/editor/web/arch/plain.js",
  "apps/editor/web/arch/start.js",
  "apps/editor/web/arch/residency.js",
  "apps/editor/web/arch/timeline.js",
  "tools/mcp_server/orbit_editor_mcp_server.py",
  "docs/ORBIT_MCP.md",
  "cmake/OrbitDependencies.cmake",
]
symbols = ["HtmlPanelsUi", "HtmlPanelSpec", "WebViewHost", "HandlePageMessage", "ResolveInside", "ChangeKind::WebAsset"]
invariants = [
  "A page reaches Studio only through window.orbit.rpc, which dispatches to the same rpc::Dispatcher as the TCP server and MCP tools; there is no second command surface.",
  "The page origin is https://orbit.local/ mapped to the panel folder; navigation anywhere else is cancelled and new windows are suppressed. Messages from another origin are dropped.",
  "html.panel_read can only read inside a declared mount (ResolveInside refuses absolute, drive and .. paths) and at most 8 MiB.",
  "WebView2 is a native child window: HtmlPanelsUi positions it from the panel's ImGui content rect every drawn frame and hides it in EndFrame when its panel was not drawn.",
  "Files under a web/ folder of apps/ or engine/ classify as ChangeKind::WebAsset and reload the panel in place; no Studio rebuild. New page extensions must be added to ChangeClassifier and its test.",
  "Without the WebView2 runtime or WebView2Loader.dll a panel degrades to an open-in-browser button; Studio itself never fails to start.",
  "The WebView2 SDK is fetched at configure time (ORBIT_ENABLE_WEBVIEW2, pinned package and SHA-256); only COM headers are used and the loader DLL is resolved at run time.",
]
related = ["/editor/studio-ui/planning", "/editor/mcp-rpc", "/rules/hot-iteration"]
depends_on = ["/foundation/rpc"]
verify = [
  "ctest -R Orbit.WebBridge and the change-classifier test.",
  "Open Planning workspace, confirm the Architecture tab renders the canvas; edit apps/editor/web/architecture_canvas.html and confirm it reloads without a rebuild.",
  "Over MCP: orbit_html_panel_list, orbit_html_panel_open('architecture'), orbit_html_panel_read('architecture', 'graph.json', mount='data').",
]
verified = ""

[[diagnose]]
symptom = "an HTML panel is blank or shows the open-in-browser fallback"
steps = [
  "Call html.panel_list: webview_available=false means the WebView2 runtime or WebView2Loader.dll (beside Orbit.exe) was not found; the panel shows LastError.",
  "hosted/ready/url show whether the view was created and loaded https://orbit.local/<page>.",
]

[[diagnose]]
symptom = "saving a page does not reload the panel"
steps = [
  "The path must contain /web/ under apps/ or engine/ and its extension must be in ChangeClassifier; check the log for 'HTML panel ... reloaded'.",
  "Call html.panel_reload to force it.",
]
+++

## Shape

`engine/web_view` is UI-agnostic: `WebBridge` holds the page protocol (bootstrap script, request validation, path containment) and `WebViewHost` wraps WebView2. `HtmlPanelsUi` in `studio_ui` owns the panels: each is an ordinary dock panel whose body is empty because the native web window covers it.

Pages are plain files under `apps/editor/web/` (override with `ORBIT_WEB_ROOT`). A page may keep state in `localStorage`; it is per panel origin.

## Page API

```js
const plan = await orbit.rpc('planning.list');
orbit.on('plan.changed', data => { /* html.panel_emit */ });
```

## The Architecture panel

`architecture_canvas.html` is the shell: tab bar, the Map canvas and the data load (`graph.json` through the `data` mount). The other tabs are plain scripts in `apps/editor/web/arch/` that register themselves with `Arch.register`: **Start here** (`start.js`, text in `plain.js`), **Residency** (`residency.js`: CPU, bridge and GPU columns with the hand-offs drawn between them) and **Timeline** (`timeline.js`: cadence buckets, one frame in order, event cascade). `plain.js` holds an everyday-language name and sentence for every system id; a system missing there falls back to its technical label. Saving any of these files reloads the panel.

+++
path = "/editor/web-view"
title = "Web view: WebView2 host and page bridge"
kind = "subsystem"
status = "experimental"
summary = "UI-agnostic module that hosts HTML pages in Microsoft WebView2 child windows and validates the page-to-RPC message protocol. It owns neither panels nor docking (studio_ui/HtmlPanelsUi does) and has no ImGui dependency."
owner_module = "OrbitWebView"
keywords = ["webview2", "web bridge", "orbit.rpc", "html", "bootstrap script", "ResolveInside"]
sources = [
  "engine/web_view/include/orbit/web_view/WebBridge.hpp",
  "engine/web_view/src/WebBridge.cpp",
  "engine/web_view/include/orbit/web_view/WebViewHost.hpp",
  "engine/web_view/src/WebViewHost.cpp",
  "engine/web_view/CMakeLists.txt"
]
symbols = ["BridgeLimits", "WebViewSpec", "HandlePageMessage", "ResolveInside", "BootstrapScript", "WebViewHost"]
invariants = [
  "WebBridge is pure and unit-tested (Orbit.WebBridge): malformed, oversized, batched or policy-denied page messages get JSON-RPC errors and never reach the dispatcher; a denied call still echoes the page's id.",
  "ResolveInside refuses absolute, drive-letter and .. escapes; it is the only way a page-supplied path becomes a file path.",
  "WebViewHost compiles to a stub (Available() false) without ORBIT_HAS_WEBVIEW2; the loader DLL is loaded at run time, so linking never depends on it.",
  "Navigation is restricted to https://orbit.local/ (virtual host mapped to the view folder); other URLs and new windows are cancelled.",
]
related = ["/editor/studio-ui/html-panels", "/editor/mcp-rpc"]
depends_on = ["/foundation/rpc"]
verify = ["ctest -R Orbit.WebBridge"]
verified = ""
+++

`WebViewHost` creates one WebView2 controller per view as a child of the Studio window, asynchronously; `SetBounds`/`SetVisible` are cheap and may be called every frame. Pages see `window.orbit` (`rpc`, `on`, `off`) injected by `BootstrapScript`. See the panel manager in `/editor/studio-ui/html-panels` for how views are placed and hot-reloaded.

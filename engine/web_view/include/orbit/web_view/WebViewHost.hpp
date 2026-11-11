#pragma once

#include <orbit/core/Types.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace orbit::web_view
{
using WebViewId = u32;

struct WebViewSpec
{
    // Folder served to the page as https://orbit.local/ (virtual host mapping:
    // the page can load its own relative scripts, styles and data from it and
    // nothing else on disk).
    std::filesystem::path folder;
    // File inside `folder` the page starts at.
    std::string startPage{"index.html"};
};

struct WebViewCallbacks
{
    // A page posted a message (the UTF-8 JSON string passed to
    // window.chrome.webview.postMessage). Runs on the UI thread.
    std::function<void(WebViewId, std::string message)> onMessage;
    // The page finished its first navigation. Runs on the UI thread.
    std::function<void(WebViewId)> onLoaded;
};

// Hosts WebView2 browser views as child windows of the Studio window. UI
// thread only: WebView2 is a single-threaded-apartment COM component and
// relies on the window's message loop. Without WebView2 (non-Windows build,
// missing SDK at build time, or no runtime installed) every call is a no-op
// and Available() is false, so callers fall back to opening the page in the
// default browser.
class WebViewHost
{
public:
    // `parentWindow` is the native window handle (HWND) the views are children of.
    WebViewHost(void* parentWindow, WebViewCallbacks callbacks);
    ~WebViewHost();

    WebViewHost(const WebViewHost&) = delete;
    WebViewHost& operator=(const WebViewHost&) = delete;

    // True when the loader and a runtime were found. Cheap after the first call.
    [[nodiscard]] static bool RuntimeAvailable();
    [[nodiscard]] bool Available() const noexcept;
    // Why the host is unavailable or the last operation failed; empty when fine.
    [[nodiscard]] std::string LastError() const;

    // Starts creating a view and returns its id at once; the browser comes up
    // asynchronously and the latest bounds, visibility and page are applied
    // when it does. Returns 0 when WebView2 is unavailable.
    [[nodiscard]] WebViewId Create(const WebViewSpec& spec);
    void Destroy(WebViewId id);

    // Client-area pixels of the parent window (the same units ImGui uses).
    void SetBounds(WebViewId id, i32 x, i32 y, i32 width, i32 height);
    void SetVisible(WebViewId id, bool visible);
    void Reload(WebViewId id);
    // Posts a JSON text message to the page (received by orbit.on / rpc).
    void PostJson(WebViewId id, std::string_view json);

    [[nodiscard]] bool Ready(WebViewId id) const;
    [[nodiscard]] std::string CurrentUrl(WebViewId id) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::web_view

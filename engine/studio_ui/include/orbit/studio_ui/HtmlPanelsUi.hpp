#pragma once

#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/rpc/JsonRpc.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::studio_ui
{
// A named folder a page may read through html.panel_read (and nothing else).
struct HtmlMount
{
    std::string name;
    std::filesystem::path folder;
};

struct HtmlPanelSpec
{
    // Stable identifier used by RPC/MCP ("architecture"); also seeds the PanelId.
    std::string id;
    std::string title;
    // Folder served to the page as https://orbit.local/ and the page inside it.
    std::filesystem::path folder;
    std::string page{"index.html"};
    std::vector<HtmlMount> mounts;
    editor_ui::DockRegion dock{editor_ui::DockRegion::Center};
    i32 dockOrder{50};
};

// Studio panels that host an HTML page (Microsoft WebView2) with a JSON-RPC
// bridge: a page calls window.orbit.rpc(method, params) and reaches the same
// RPC surface the MCP tools reach. The page folder is hot: saving a web asset
// reloads the panel without rebuilding Studio (see ReloadForPath). Without
// WebView2 a panel offers "open in browser" instead.
class HtmlPanelsUi
{
public:
    HtmlPanelsUi();
    ~HtmlPanelsUi();

    HtmlPanelsUi(const HtmlPanelsUi&) = delete;
    HtmlPanelsUi& operator=(const HtmlPanelsUi&) = delete;

    // `nativeWindow` is the Studio window handle (HWND); `dispatcher` answers the
    // pages' RPC calls; `webRoot` is the folder holding Studio's own pages
    // (apps/editor/web). All must outlive this object.
    void Attach(
        editor_ui::EditorUi& ui,
        void* nativeWindow,
        const rpc::Dispatcher& dispatcher,
        std::filesystem::path webRoot);

    void RegisterRpc(rpc::Dispatcher& dispatcher);

    // Declares a panel, registered closed (or open). Declaring an existing id
    // replaces its spec and reloads it.
    void Define(HtmlPanelSpec spec, bool open = false);

    [[nodiscard]] bool Open(std::string_view id, bool focus = true);
    [[nodiscard]] bool Close(std::string_view id);

    // Hides web views whose panel was not drawn this frame. Call once per frame
    // after the shell is drawn.
    void EndFrame();

    // A web asset at `path` changed on disk: reload every panel serving it.
    void ReloadForPath(const std::filesystem::path& path);

    // Pushes {event, data} to a panel's page (orbit.on).
    void Emit(std::string_view id, std::string_view event, std::string_view dataJson);

    [[nodiscard]] const std::filesystem::path& WebRoot() const noexcept;

    // Called when a panel cannot host a web view and the user asks to open the
    // page externally.
    void SetOpenExternalHandler(std::function<void(const std::filesystem::path&)> handler);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::studio_ui

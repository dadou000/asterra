#include <orbit/studio_ui/HtmlPanelsUi.hpp>

#include <orbit/core/Log.hpp>
#include <orbit/web_view/WebBridge.hpp>
#include <orbit/web_view/WebViewHost.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>

namespace orbit::studio_ui
{
namespace
{
using rpc::Value;

constexpr std::uintmax_t kMaxReadBytes = 8U * 1024U * 1024U;

[[nodiscard]] u64 Fnv1a(const std::string_view text) noexcept
{
    u64 hash = 1469598103934665603ULL;
    for (const char c : text)
        hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
    return hash;
}

[[nodiscard]] editor_ui::PanelId PanelIdFor(const std::string_view id) noexcept
{
    return {.high = 0x4f5242495448544cULL, .low = Fnv1a(id)};
}

[[nodiscard]] const Value& Object(const Value& params)
{
    if (!params.IsObject())
        throw rpc::Error(-32602, "params must be an object");
    return params;
}

[[nodiscard]] std::string RequiredString(const Value& params, const char* key)
{
    const Value* value = params.Find(key);
    if (value == nullptr || !value->IsString() || value->AsString().empty())
        throw rpc::Error(-32602, std::string(key) + " must be a non-empty string");
    return value->AsString();
}

[[nodiscard]] std::string OptionalString(const Value& params, const char* key)
{
    const Value* value = params.Find(key);
    if (value == nullptr || value->IsNull())
        return {};
    if (!value->IsString())
        throw rpc::Error(-32602, std::string(key) + " must be a string");
    return value->AsString();
}
} // namespace

struct HtmlPanelsUi::Impl
{
    struct Panel
    {
        HtmlPanelSpec spec;
        web_view::WebViewId view{0U};
        u64 drawnFrame{~0ULL};
        bool wasVisible{false};
        editor_ui::UiRect lastRect{};
    };

    editor_ui::EditorUi* ui{nullptr};
    const rpc::Dispatcher* dispatcher{nullptr};
    std::filesystem::path webRoot;
    std::unique_ptr<web_view::WebViewHost> host;
    std::map<std::string, Panel, std::less<>> panels;
    std::function<void(const std::filesystem::path&)> openExternal;
    u64 frame{0U};

    [[nodiscard]] Panel* FindById(const std::string_view id)
    {
        const auto found = panels.find(id);
        return found == panels.end() ? nullptr : &found->second;
    }

    [[nodiscard]] Panel* FindByView(const web_view::WebViewId view)
    {
        for (auto& [id, panel] : panels)
            if (panel.view == view && view != 0U)
                return &panel;
        return nullptr;
    }

    void OnMessage(const web_view::WebViewId view, const std::string& message)
    {
        Panel* panel = FindByView(view);
        if (panel == nullptr || dispatcher == nullptr || host == nullptr)
            return;
        const auto response = web_view::HandlePageMessage(
            message,
            [this](const std::string_view request) { return dispatcher->Dispatch(request); });
        if (response.has_value())
            host->PostJson(view, *response);
    }

    void Draw(const std::string& id, editor_ui::PanelContext& context)
    {
        Panel* panel = FindById(id);
        if (panel == nullptr)
            return;
        if (host == nullptr || !host->Available())
        {
            context.Text("This panel needs the Microsoft WebView2 runtime, which was not found.");
            if (host != nullptr && !host->LastError().empty())
                context.MutedText(host->LastError());
            if (context.Button(("Open page in browser##html-" + id).c_str()) && openExternal)
            {
                try
                {
                    openExternal(panel->spec.folder / panel->spec.page);
                }
                catch (const std::exception& error)
                {
                    log::Warning(std::string("HTML panel: ") + error.what());
                }
            }
            return;
        }
        if (panel->view == 0U)
            panel->view = host->Create({.folder = panel->spec.folder, .startPage = panel->spec.page});

        // Nothing is drawn over the area: the native window covers it. Read the
        // rectangle first so it is the panel's full content area.
        const editor_ui::UiRect rect = context.ContentScreenRect();
        if (panel->view != 0U)
        {
            host->SetBounds(
                panel->view,
                static_cast<i32>(std::lround(rect.x)),
                static_cast<i32>(std::lround(rect.y)),
                static_cast<i32>(std::lround(rect.width)),
                static_cast<i32>(std::lround(rect.height)));
            panel->drawnFrame = frame;
            panel->lastRect = rect;
        }
    }

    void Declare(HtmlPanelSpec spec, const bool open)
    {
        if (ui == nullptr)
            return;
        const std::string id = spec.id;
        const bool replacing = panels.contains(id);
        Panel& panel = panels[id];
        if (panel.view != 0U && host != nullptr)
        {
            host->Destroy(panel.view);
            panel.view = 0U;
        }
        panel.spec = std::move(spec);
        const editor_ui::PanelId panelId = PanelIdFor(id);
        const bool wasOpen = replacing && ui->PanelOpen(panelId);
        ui->UpsertPanel({
            .id = panelId,
            .title = panel.spec.title,
            .defaultOpen = open,
            .defaultDock = panel.spec.dock,
            .dockOrder = panel.spec.dockOrder,
            .minSize = {320.0F, 240.0F},
            .defaultSize = {1000.0F, 700.0F},
            .draw = [this, id](editor_ui::PanelContext& context) { Draw(id, context); }
        });
        if (wasOpen)
            static_cast<void>(ui->SetPanelOpen(panelId, true));
    }

    [[nodiscard]] Value Describe(const std::string& id, const Panel& panel) const
    {
        const bool open = ui != nullptr && ui->PanelOpen(PanelIdFor(id));
        Value::Array mounts;
        for (const HtmlMount& mount : panel.spec.mounts)
            mounts.push_back(Value::Object{
                {"name", mount.name}, {"folder", mount.folder.generic_string()}});
        return Value::Object{
            {"id", id},
            {"title", panel.spec.title},
            {"page", panel.spec.page},
            {"folder", panel.spec.folder.generic_string()},
            {"open", open},
            {"hosted", panel.view != 0U},
            {"ready", host != nullptr && panel.view != 0U && host->Ready(panel.view)},
            {"visible", panel.wasVisible},
            {"bounds", Value::Array{static_cast<i64>(std::lround(panel.lastRect.x)), static_cast<i64>(std::lround(panel.lastRect.y)),
                static_cast<i64>(std::lround(panel.lastRect.width)), static_cast<i64>(std::lround(panel.lastRect.height))}},
            {"url", host != nullptr && panel.view != 0U ? host->CurrentUrl(panel.view) : std::string{}},
            {"mounts", std::move(mounts)}};
    }
};

HtmlPanelsUi::HtmlPanelsUi() : impl_(std::make_unique<Impl>()) {}
HtmlPanelsUi::~HtmlPanelsUi() = default;

void HtmlPanelsUi::Attach(
    editor_ui::EditorUi& ui,
    void* const nativeWindow,
    const rpc::Dispatcher& dispatcher,
    std::filesystem::path webRoot)
{
    impl_->ui = &ui;
    impl_->dispatcher = &dispatcher;
    impl_->webRoot = std::move(webRoot);
    Impl* impl = impl_.get();
    impl_->host = std::make_unique<web_view::WebViewHost>(
        nativeWindow,
        web_view::WebViewCallbacks{
            .onMessage = [impl](const web_view::WebViewId view, std::string message)
            { impl->OnMessage(view, message); },
            .onLoaded = {}});
}

void HtmlPanelsUi::Define(HtmlPanelSpec spec, const bool open)
{
    impl_->Declare(std::move(spec), open);
}

bool HtmlPanelsUi::Open(const std::string_view id, const bool focus)
{
    if (impl_->ui == nullptr || impl_->FindById(id) == nullptr)
        return false;
    const editor_ui::PanelId panelId = PanelIdFor(id);
    return focus ? impl_->ui->FocusPanel(panelId) : impl_->ui->SetPanelOpen(panelId, true);
}

bool HtmlPanelsUi::Close(const std::string_view id)
{
    if (impl_->ui == nullptr || impl_->FindById(id) == nullptr)
        return false;
    return impl_->ui->SetPanelOpen(PanelIdFor(id), false);
}

void HtmlPanelsUi::EndFrame()
{
    if (impl_->host != nullptr)
        for (auto& [id, panel] : impl_->panels)
        {
            if (panel.view == 0U)
                continue;
            const bool visible = panel.drawnFrame == impl_->frame;
            if (visible != panel.wasVisible)
            {
                impl_->host->SetVisible(panel.view, visible);
                panel.wasVisible = visible;
            }
        }
    ++impl_->frame;
}

void HtmlPanelsUi::ReloadForPath(const std::filesystem::path& path)
{
    if (impl_->host == nullptr)
        return;
    std::error_code error;
    const std::filesystem::path changed = std::filesystem::weakly_canonical(path, error);
    if (error)
        return;
    for (auto& [id, panel] : impl_->panels)
    {
        if (panel.view == 0U)
            continue;
        const std::filesystem::path folder =
            std::filesystem::weakly_canonical(panel.spec.folder, error);
        if (error)
            continue;
        const auto relative = changed.lexically_relative(folder);
        if (!relative.empty() && !relative.generic_string().starts_with(".."))
        {
            impl_->host->Reload(panel.view);
            log::Info("HTML panel '" + id + "' reloaded: " + path.filename().string());
        }
    }
}

void HtmlPanelsUi::Emit(
    const std::string_view id, const std::string_view event, const std::string_view dataJson)
{
    Impl::Panel* panel = impl_->FindById(id);
    if (panel == nullptr || panel->view == 0U || impl_->host == nullptr)
        return;
    impl_->host->PostJson(panel->view, web_view::MakeEventMessage(event, dataJson));
}

const std::filesystem::path& HtmlPanelsUi::WebRoot() const noexcept
{
    return impl_->webRoot;
}

void HtmlPanelsUi::SetOpenExternalHandler(std::function<void(const std::filesystem::path&)> handler)
{
    impl_->openExternal = std::move(handler);
}

void HtmlPanelsUi::RegisterRpc(rpc::Dispatcher& dispatcher)
{
    Impl* impl = impl_.get();

    dispatcher.Register({
        .name = "html.panel_list",
        .description = "List HTML panels (id, title, page, folder, open, hosted, ready, url, mounts).",
        .mutating = false
    }, [impl](const Value&)
    {
        Value::Array panels;
        for (const auto& [id, panel] : impl->panels)
            panels.push_back(impl->Describe(id, panel));
        return Value::Object{
            {"panels", std::move(panels)},
            {"webview_available", impl->host != nullptr && impl->host->Available()},
            {"web_root", impl->webRoot.generic_string()}};
    });

    dispatcher.Register({
        .name = "html.panel_open",
        .description =
            "Open (and focus) an HTML panel by id. With a page it defines the panel first: page is "
            "relative to folder (default: Studio's web root, apps/editor/web); mounts is an optional "
            "array of {name, folder} data folders the page may read through html.panel_read.",
        .mutating = true
    }, [impl](const Value& input)
    {
        const Value& params = Object(input);
        const std::string id = RequiredString(params, "id");
        const std::string page = OptionalString(params, "page");
        if (!page.empty())
        {
            HtmlPanelSpec spec;
            spec.id = id;
            const std::string title = OptionalString(params, "title");
            spec.title = title.empty() ? id : title;
            const std::string folder = OptionalString(params, "folder");
            spec.folder = folder.empty() ? impl->webRoot : std::filesystem::path(folder);
            spec.page = page;
            std::error_code error;
            if (!std::filesystem::is_directory(spec.folder, error))
                throw rpc::Error(-32602, "folder does not exist: " + spec.folder.generic_string());
            const auto resolved = web_view::ResolveInside(spec.folder, page);
            if (!resolved || !std::filesystem::is_regular_file(*resolved, error))
                throw rpc::Error(-32602, "page does not exist inside folder: " + page);
            if (const Value* mounts = params.Find("mounts"); mounts != nullptr && mounts->IsArray())
                for (const Value& entry : mounts->AsArray())
                    if (entry.IsObject() && entry.Find("name") != nullptr && entry.Find("folder") != nullptr &&
                        entry.Find("name")->IsString() && entry.Find("folder")->IsString())
                        spec.mounts.push_back({entry.Find("name")->AsString(),
                            std::filesystem::path(entry.Find("folder")->AsString())});
            impl->Declare(std::move(spec), false);
        }
        else if (impl->FindById(id) == nullptr)
            throw rpc::Error(-32602, "unknown html panel '" + id + "'; pass page to define it");
        const editor_ui::PanelId panelId = PanelIdFor(id);
        if (!impl->ui->FocusPanel(panelId))
            throw rpc::Error(-32603, "the panel could not be opened");
        return impl->Describe(id, *impl->FindById(id));
    });

    dispatcher.Register({
        .name = "html.panel_close",
        .description = "Close an HTML panel (its page keeps its state until reloaded).",
        .mutating = true
    }, [impl](const Value& input)
    {
        const std::string id = RequiredString(Object(input), "id");
        Impl::Panel* panel = impl->FindById(id);
        if (panel == nullptr)
            throw rpc::Error(-32602, "unknown html panel '" + id + "'");
        static_cast<void>(impl->ui->SetPanelOpen(PanelIdFor(id), false));
        return impl->Describe(id, *panel);
    });

    dispatcher.Register({
        .name = "html.panel_reload",
        .description = "Reload an HTML panel's page from disk.",
        .mutating = true
    }, [impl](const Value& input)
    {
        const std::string id = RequiredString(Object(input), "id");
        Impl::Panel* panel = impl->FindById(id);
        if (panel == nullptr)
            throw rpc::Error(-32602, "unknown html panel '" + id + "'");
        if (panel->view != 0U && impl->host != nullptr)
            impl->host->Reload(panel->view);
        return impl->Describe(id, *panel);
    });

    dispatcher.Register({
        .name = "html.panel_read",
        .description =
            "Read a UTF-8 text file (at most 8 MiB) from an HTML panel's mount (or its own folder when "
            "mount is omitted). Returns text, size and mtime (nanoseconds). Paths cannot leave the mount.",
        .mutating = false
    }, [impl](const Value& input)
    {
        const Value& params = Object(input);
        const std::string id = RequiredString(params, "id");
        const std::string path = RequiredString(params, "path");
        const std::string mountName = OptionalString(params, "mount");
        Impl::Panel* panel = impl->FindById(id);
        if (panel == nullptr)
            throw rpc::Error(-32602, "unknown html panel '" + id + "'");
        std::filesystem::path root = panel->spec.folder;
        if (!mountName.empty())
        {
            const auto mount = std::ranges::find_if(panel->spec.mounts,
                [&](const HtmlMount& m) { return m.name == mountName; });
            if (mount == panel->spec.mounts.end())
                throw rpc::Error(-32602, "unknown mount '" + mountName + "'");
            root = mount->folder;
        }
        const auto resolved = web_view::ResolveInside(root, path);
        if (!resolved)
            throw rpc::Error(-32602, "path leaves the mount");
        std::error_code error;
        if (!std::filesystem::is_regular_file(*resolved, error))
            throw rpc::Error(-32602, "no such file: " + path);
        const std::uintmax_t size = std::filesystem::file_size(*resolved, error);
        if (error || size > kMaxReadBytes)
            throw rpc::Error(-32602, "file is too large to read through the bridge");
        std::ifstream stream(*resolved, std::ios::binary);
        std::ostringstream text;
        text << stream.rdbuf();
        const auto stamp = std::filesystem::last_write_time(*resolved, error);
        return Value::Object{
            {"text", text.str()},
            {"size", static_cast<i64>(size)},
            {"mtime", static_cast<i64>(stamp.time_since_epoch().count())}};
    });

    dispatcher.Register({
        .name = "html.panel_emit",
        .description = "Push {event, data} to an HTML panel's page (delivered to orbit.on(event, fn)); data is JSON text.",
        .mutating = true
    }, [impl](const Value& input)
    {
        const Value& params = Object(input);
        const std::string id = RequiredString(params, "id");
        const std::string event = RequiredString(params, "event");
        Impl::Panel* panel = impl->FindById(id);
        if (panel == nullptr)
            throw rpc::Error(-32602, "unknown html panel '" + id + "'");
        std::string data;
        if (const Value* value = params.Find("data"); value != nullptr)
            data = rpc::Serialize(*value);
        if (panel->view != 0U && impl->host != nullptr)
            impl->host->PostJson(panel->view, web_view::MakeEventMessage(event, data));
        return Value::Object{{"delivered", panel->view != 0U}};
    });
}
} // namespace orbit::studio_ui

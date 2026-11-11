#include <orbit/web_view/WebViewHost.hpp>

#include <orbit/web_view/WebBridge.hpp>

#include <map>
#include <string>
#include <utility>

#if defined(_WIN32) && defined(ORBIT_HAS_WEBVIEW2)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wrl.h>

#include <WebView2.h>

#include <orbit/core/Log.hpp>

namespace orbit::web_view
{
namespace
{
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

constexpr const wchar_t* kHostName = L"orbit.local";
constexpr const wchar_t* kOrigin = L"https://orbit.local/";

using CreateEnvironmentFn = HRESULT(STDAPICALLTYPE*)(
    PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
using AvailableVersionFn = HRESULT(STDAPICALLTYPE*)(PCWSTR, LPWSTR*);

[[nodiscard]] std::wstring ToWide(const std::string_view text)
{
    if (text.empty())
        return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        result.data(), count);
    return result;
}

[[nodiscard]] std::string ToUtf8(const std::wstring_view text)
{
    if (text.empty())
        return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        result.data(), count, nullptr, nullptr);
    return result;
}

// WebView2Loader.dll ships beside Studio (copied by the build); fall back to
// the normal DLL search path so a system-wide copy also works.
[[nodiscard]] HMODULE LoadLoader()
{
    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) != 0U)
    {
        std::wstring beside(exePath);
        const auto slash = beside.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
        {
            beside.resize(slash + 1U);
            beside += L"WebView2Loader.dll";
            if (HMODULE module = LoadLibraryW(beside.c_str()))
                return module;
        }
    }
    return LoadLibraryW(L"WebView2Loader.dll");
}

[[nodiscard]] std::wstring UserDataFolder()
{
    wchar_t local[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    std::wstring folder = length > 0U && length < MAX_PATH
        ? std::wstring(local) : std::wstring(L".");
    folder += L"\\Orbit\\WebView2";
    return folder;
}

[[nodiscard]] bool StartsWith(const std::wstring_view text, const std::wstring_view prefix)
{
    return text.size() >= prefix.size() && text.compare(0U, prefix.size(), prefix) == 0;
}
} // namespace

struct WebViewHost::Impl
{
    struct View
    {
        WebViewId id{0U};
        WebViewSpec spec;
        ComPtr<ICoreWebView2Controller> controller;
        ComPtr<ICoreWebView2> webview;
        EventRegistrationToken messageToken{};
        EventRegistrationToken navigationToken{};
        EventRegistrationToken loadedToken{};
        EventRegistrationToken newWindowToken{};
        RECT bounds{0, 0, 0, 0};
        bool hasBounds{false};
        bool visible{false};
        bool ready{false};
        bool loadedReported{false};
        bool creating{false};
        std::wstring url;
    };

    HWND parent{nullptr};
    WebViewCallbacks callbacks;
    HMODULE loader{nullptr};
    CreateEnvironmentFn createEnvironment{nullptr};
    ComPtr<ICoreWebView2Environment> environment;
    bool environmentRequested{false};
    bool environmentFailed{false};
    std::string lastError;
    std::map<WebViewId, std::unique_ptr<View>> views;
    WebViewId nextId{1U};
    // Async completions outlive the host's call stack; they check this before
    // touching `this`.
    std::shared_ptr<bool> alive{std::make_shared<bool>(true)};

    void Fail(std::string message)
    {
        lastError = std::move(message);
        log::Warning("WebView2: " + lastError);
    }

    void EnsureEnvironment()
    {
        if (environment || environmentRequested || environmentFailed)
            return;
        loader = LoadLoader();
        if (loader == nullptr)
        {
            environmentFailed = true;
            Fail("WebView2Loader.dll was not found beside Studio.");
            return;
        }
        createEnvironment = reinterpret_cast<CreateEnvironmentFn>(
            GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions"));
        if (createEnvironment == nullptr)
        {
            environmentFailed = true;
            Fail("WebView2Loader.dll does not export CreateCoreWebView2EnvironmentWithOptions.");
            return;
        }
        // WebView2 needs a single-threaded apartment on this thread; the window
        // message loop already runs here. Another apartment mode is a hard stop.
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (apartment == RPC_E_CHANGED_MODE)
        {
            environmentFailed = true;
            Fail("The UI thread is not a single-threaded COM apartment.");
            return;
        }

        environmentRequested = true;
        const std::wstring userData = UserDataFolder();
        const auto flag = alive;
        const HRESULT started = createEnvironment(
            nullptr, userData.c_str(), nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [this, flag](HRESULT result, ICoreWebView2Environment* created) -> HRESULT
                {
                    if (!*flag)
                        return S_OK;
                    environmentRequested = false;
                    if (FAILED(result) || created == nullptr)
                    {
                        environmentFailed = true;
                        Fail("WebView2 environment creation failed (runtime missing or blocked).");
                        return S_OK;
                    }
                    environment = created;
                    for (auto& [id, view] : views)
                        if (!view->controller && !view->creating)
                            CreateController(*view);
                    return S_OK;
                }).Get());
        if (FAILED(started))
        {
            environmentRequested = false;
            environmentFailed = true;
            Fail("CreateCoreWebView2EnvironmentWithOptions failed.");
        }
    }

    void ApplyState(View& view)
    {
        if (!view.controller)
            return;
        if (view.hasBounds)
            view.controller->put_Bounds(view.bounds);
        view.controller->put_IsVisible(view.visible && view.hasBounds ? TRUE : FALSE);
    }

    void CreateController(View& view)
    {
        view.creating = true;
        const WebViewId id = view.id;
        const auto flag = alive;
        const HRESULT started = environment->CreateCoreWebView2Controller(
            parent,
            Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [this, id, flag](HRESULT result, ICoreWebView2Controller* created) -> HRESULT
                {
                    if (!*flag)
                        return S_OK;
                    const auto found = views.find(id);
                    if (found == views.end())
                    {
                        if (created != nullptr)
                            created->Close();
                        return S_OK;
                    }
                    View& target = *found->second;
                    target.creating = false;
                    if (FAILED(result) || created == nullptr)
                    {
                        Fail("WebView2 controller creation failed.");
                        return S_OK;
                    }
                    target.controller = created;
                    target.controller->get_CoreWebView2(&target.webview);
                    if (!target.webview)
                    {
                        Fail("WebView2 controller has no browser.");
                        return S_OK;
                    }
                    Configure(target);
                    return S_OK;
                }).Get());
        if (FAILED(started))
        {
            view.creating = false;
            Fail("CreateCoreWebView2Controller failed.");
        }
    }

    void Configure(View& view)
    {
        ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(view.webview->get_Settings(&settings)) && settings)
        {
            settings->put_IsStatusBarEnabled(FALSE);
            settings->put_IsZoomControlEnabled(FALSE);
            settings->put_AreDefaultContextMenusEnabled(TRUE);
            // Developer tools stay on: these are authoring surfaces.
            settings->put_AreDevToolsEnabled(TRUE);
        }

        const std::wstring bootstrap = ToWide(BootstrapScript());
        view.webview->AddScriptToExecuteOnDocumentCreated(bootstrap.c_str(), nullptr);

        // The page sees only its own folder, as https://orbit.local/.
        ComPtr<ICoreWebView2_3> webview3;
        if (SUCCEEDED(view.webview.As(&webview3)) && webview3)
        {
            webview3->SetVirtualHostNameToFolderMapping(
                kHostName, view.spec.folder.wstring().c_str(),
                COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
        }

        const WebViewId id = view.id;
        const auto flag = alive;
        view.webview->add_WebMessageReceived(
            Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                [this, id, flag](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT
                {
                    if (!*flag || !callbacks.onMessage)
                        return S_OK;
                    // Only our own origin may talk to Studio.
                    LPWSTR source = nullptr;
                    args->get_Source(&source);
                    const bool trusted = source != nullptr &&
                        StartsWith(source, kOrigin);
                    if (source != nullptr)
                        CoTaskMemFree(source);
                    if (!trusted)
                        return S_OK;
                    LPWSTR message = nullptr;
                    if (SUCCEEDED(args->TryGetWebMessageAsString(&message)) && message != nullptr)
                    {
                        callbacks.onMessage(id, ToUtf8(message));
                        CoTaskMemFree(message);
                    }
                    return S_OK;
                }).Get(),
            &view.messageToken);

        // Lock navigation to the page's own origin: a link or script must not
        // carry the bridge to an external site.
        view.webview->add_NavigationStarting(
            Callback<ICoreWebView2NavigationStartingEventHandler>(
                [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT
                {
                    LPWSTR uri = nullptr;
                    args->get_Uri(&uri);
                    const bool allowed = uri != nullptr &&
                        (StartsWith(uri, kOrigin) || StartsWith(uri, L"about:blank"));
                    if (uri != nullptr)
                        CoTaskMemFree(uri);
                    if (!allowed)
                        args->put_Cancel(TRUE);
                    return S_OK;
                }).Get(),
            &view.navigationToken);

        view.webview->add_NewWindowRequested(
            Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT
                {
                    args->put_Handled(TRUE);
                    return S_OK;
                }).Get(),
            &view.newWindowToken);

        view.webview->add_NavigationCompleted(
            Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [this, id, flag](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT
                {
                    if (!*flag)
                        return S_OK;
                    const auto found = views.find(id);
                    if (found != views.end() && !found->second->loadedReported)
                    {
                        found->second->loadedReported = true;
                        if (callbacks.onLoaded)
                            callbacks.onLoaded(id);
                    }
                    return S_OK;
                }).Get(),
            &view.loadedToken);

        view.ready = true;
        ApplyState(view);
        view.url = std::wstring(kOrigin) + ToWide(view.spec.startPage);
        view.webview->Navigate(view.url.c_str());
    }

    void Teardown(View& view)
    {
        if (view.webview)
        {
            view.webview->remove_WebMessageReceived(view.messageToken);
            view.webview->remove_NavigationStarting(view.navigationToken);
            view.webview->remove_NavigationCompleted(view.loadedToken);
            view.webview->remove_NewWindowRequested(view.newWindowToken);
        }
        if (view.controller)
            view.controller->Close();
        view.webview.Reset();
        view.controller.Reset();
    }
};

WebViewHost::WebViewHost(void* parentWindow, WebViewCallbacks callbacks)
    : impl_(std::make_unique<Impl>())
{
    impl_->parent = static_cast<HWND>(parentWindow);
    impl_->callbacks = std::move(callbacks);
}

WebViewHost::~WebViewHost()
{
    *impl_->alive = false;
    for (auto& [id, view] : impl_->views)
        impl_->Teardown(*view);
    impl_->views.clear();
    impl_->environment.Reset();
}

bool WebViewHost::RuntimeAvailable()
{
    static const bool available = []
    {
        const HMODULE module = LoadLoader();
        if (module == nullptr)
            return false;
        const auto version = reinterpret_cast<AvailableVersionFn>(
            GetProcAddress(module, "GetAvailableCoreWebView2BrowserVersionString"));
        if (version == nullptr)
            return false;
        LPWSTR text = nullptr;
        const HRESULT result = version(nullptr, &text);
        const bool found = SUCCEEDED(result) && text != nullptr && text[0] != L'\0';
        if (text != nullptr)
            CoTaskMemFree(text);
        return found;
    }();
    return available;
}

bool WebViewHost::Available() const noexcept
{
    return !impl_->environmentFailed && RuntimeAvailable();
}

std::string WebViewHost::LastError() const
{
    return impl_->lastError;
}

WebViewId WebViewHost::Create(const WebViewSpec& spec)
{
    if (!Available())
        return 0U;
    impl_->EnsureEnvironment();
    if (impl_->environmentFailed)
        return 0U;
    const WebViewId id = impl_->nextId++;
    auto view = std::make_unique<Impl::View>();
    view->id = id;
    view->spec = spec;
    Impl::View& ref = *view;
    impl_->views.emplace(id, std::move(view));
    if (impl_->environment)
        impl_->CreateController(ref);
    return id;
}

void WebViewHost::Destroy(const WebViewId id)
{
    const auto found = impl_->views.find(id);
    if (found == impl_->views.end())
        return;
    impl_->Teardown(*found->second);
    impl_->views.erase(found);
}

void WebViewHost::SetBounds(const WebViewId id, const i32 x, const i32 y, const i32 width, const i32 height)
{
    const auto found = impl_->views.find(id);
    if (found == impl_->views.end())
        return;
    Impl::View& view = *found->second;
    view.bounds = RECT{x, y, x + width, y + height};
    view.hasBounds = width > 0 && height > 0;
    impl_->ApplyState(view);
}

void WebViewHost::SetVisible(const WebViewId id, const bool visible)
{
    const auto found = impl_->views.find(id);
    if (found == impl_->views.end())
        return;
    found->second->visible = visible;
    impl_->ApplyState(*found->second);
}

void WebViewHost::Reload(const WebViewId id)
{
    const auto found = impl_->views.find(id);
    if (found != impl_->views.end() && found->second->webview)
        found->second->webview->Reload();
}

void WebViewHost::PostJson(const WebViewId id, const std::string_view json)
{
    const auto found = impl_->views.find(id);
    if (found == impl_->views.end() || !found->second->webview)
        return;
    const std::wstring wide = ToWide(json);
    found->second->webview->PostWebMessageAsString(wide.c_str());
}

bool WebViewHost::Ready(const WebViewId id) const
{
    const auto found = impl_->views.find(id);
    return found != impl_->views.end() && found->second->ready;
}

std::string WebViewHost::CurrentUrl(const WebViewId id) const
{
    const auto found = impl_->views.find(id);
    return found == impl_->views.end() ? std::string{} : ToUtf8(found->second->url);
}
} // namespace orbit::web_view

#else // no WebView2: every call is a no-op and Available() is false.

namespace orbit::web_view
{
struct WebViewHost::Impl
{
};

WebViewHost::WebViewHost(void*, WebViewCallbacks)
    : impl_(std::make_unique<Impl>())
{
}

WebViewHost::~WebViewHost() = default;
bool WebViewHost::RuntimeAvailable() { return false; }
bool WebViewHost::Available() const noexcept { return false; }
std::string WebViewHost::LastError() const { return "WebView2 support was not built into this Studio."; }
WebViewId WebViewHost::Create(const WebViewSpec&) { return 0U; }
void WebViewHost::Destroy(WebViewId) {}
void WebViewHost::SetBounds(WebViewId, i32, i32, i32, i32) {}
void WebViewHost::SetVisible(WebViewId, bool) {}
void WebViewHost::Reload(WebViewId) {}
void WebViewHost::PostJson(WebViewId, std::string_view) {}
bool WebViewHost::Ready(WebViewId) const { return false; }
std::string WebViewHost::CurrentUrl(WebViewId) const { return {}; }
} // namespace orbit::web_view

#endif

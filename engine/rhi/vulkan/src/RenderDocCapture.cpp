#include <orbit/rhi/vulkan/RenderDocCapture.hpp>

#include <orbit/core/Types.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

// RegOpenKeyExA/RegEnumValueA/RegCloseKey, used to find which RenderDoc
// install (if more than one) is actually registered as the system's
// active Vulkan capture layer -- see FindRegisteredRenderDocLibraryPath.
#pragma comment(lib, "advapi32.lib")

#include "../third_party/renderdoc/renderdoc_app.h"

#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace orbit::rhi::vulkan
{
namespace
{
[[nodiscard]] RENDERDOC_API_1_6_0* Api(void* api) noexcept
{
    return static_cast<RENDERDOC_API_1_6_0*>(api);
}

[[nodiscard]] std::string ToLowerAscii(std::string text)
{
    for (char& character : text)
    {
        character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    }

    return text;
}

// A machine can have more than one RenderDoc install (a Program Files
// one and a portable/newer one, say), each with its own renderdoc.dll.
// Only ONE of them is actually wired up as the system's active Vulkan
// capture layer -- whichever manifest is registered under
// HKLM\SOFTWARE\Khronos\Vulkan\ImplicitLayers, which the Vulkan loader
// reads at vkCreateInstance time regardless of which copy this process
// happens to load via LoadLibrary. Loading a *different* copy than that
// one means TriggerCapture() talks to a module the loader never
// actually hooked -- it silently does nothing. Reading the manifest's
// own "library_path" and loading that exact file is what makes this
// process's copy and the loader's copy the same module (Windows just
// bumps the refcount on the second LoadLibrary rather than loading it
// twice), which is what actually makes captures possible.
[[nodiscard]] std::optional<std::string>
FindRegisteredRenderDocLibraryPath()
{
    HKEY key = nullptr;

    if (RegOpenKeyExA(
            HKEY_LOCAL_MACHINE,
            "SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers",
            0,
            KEY_READ,
            &key) != ERROR_SUCCESS)
    {
        return std::nullopt;
    }

    std::optional<std::string> result;

    for (DWORD index = 0;; ++index)
    {
        char valueName[MAX_PATH]{};
        DWORD valueNameSize = sizeof(valueName);

        const LSTATUS status = RegEnumValueA(
            key,
            index,
            valueName,
            &valueNameSize,
            nullptr,
            nullptr,
            nullptr,
            nullptr);

        if (status == ERROR_NO_MORE_ITEMS)
        {
            break;
        }

        if (status != ERROR_SUCCESS)
        {
            continue;
        }

        const std::string manifestPath(valueName, valueNameSize);

        if (ToLowerAscii(manifestPath).find("renderdoc") ==
            std::string::npos)
        {
            continue;
        }

        std::ifstream manifestFile(manifestPath);

        if (!manifestFile)
        {
            continue;
        }

        std::stringstream buffer;
        buffer << manifestFile.rdbuf();
        const std::string manifest = buffer.str();

        const auto keyPos = manifest.find("\"library_path\"");

        if (keyPos == std::string::npos)
        {
            continue;
        }

        const auto firstQuote =
            manifest.find('"', manifest.find(':', keyPos) + 1);
        const auto secondQuote =
            firstQuote == std::string::npos
                ? std::string::npos
                : manifest.find('"', firstQuote + 1);

        if (firstQuote == std::string::npos ||
            secondQuote == std::string::npos)
        {
            continue;
        }

        std::string libraryPath = manifest.substr(
            firstQuote + 1, secondQuote - firstQuote - 1);

        // Manifests commonly give a path relative to the manifest's
        // own directory (e.g. ".\renderdoc.dll"); resolve it there.
        const bool isAbsolute =
            libraryPath.size() > 1 && libraryPath[1] == ':';

        if (!isAbsolute)
        {
            const auto slashPos = manifestPath.find_last_of("\\/");

            const std::string directory =
                slashPos == std::string::npos
                    ? std::string{}
                    : manifestPath.substr(0, slashPos + 1);

            if (libraryPath.rfind(".\\", 0) == 0 ||
                libraryPath.rfind("./", 0) == 0)
            {
                libraryPath = libraryPath.substr(2);
            }

            libraryPath = directory + libraryPath;
        }

        result = std::move(libraryPath);
        break;
    }

    RegCloseKey(key);
    return result;
}

// RenderDoc doesn't add itself to PATH, so a bare LoadLibraryA only
// succeeds if the user copied renderdoc.dll next to the executable or
// added it to PATH themselves. Try (in order): the environment
// override, the path resolved from the registered Vulkan implicit
// layer (see above -- this is the one that actually matters for
// captures to work), RenderDoc's default install location, and
// finally the bare name in case either of those last two is true.
[[nodiscard]] HMODULE LoadRenderDocLibrary()
{
    char* customPath = nullptr;
    std::size_t customPathLength = 0;

    if (_dupenv_s(&customPath, &customPathLength, "ORBIT_RENDERDOC_DLL") ==
            0 &&
        customPath != nullptr)
    {
        HMODULE module = LoadLibraryA(customPath);
        free(customPath);

        if (module != nullptr)
        {
            return module;
        }
    }

    if (const auto registeredPath = FindRegisteredRenderDocLibraryPath())
    {
        if (HMODULE module = LoadLibraryA(registeredPath->c_str()))
        {
            return module;
        }
    }

    if (HMODULE module = LoadLibraryA(
            "C:\\Program Files\\RenderDoc\\renderdoc.dll"))
    {
        return module;
    }

    return LoadLibraryA("renderdoc.dll");
}
} // namespace

RenderDocCapture::RenderDocCapture(
    void* const module, void* const api, const bool ownsModule) noexcept
    : module_(module), api_(api), ownsModule_(ownsModule)
{
}

RenderDocCapture::~RenderDocCapture()
{
    // RenderDoc's own lifetime owns the module when we found it already
    // loaded (launched under the RenderDoc UI) -- only free it if we're
    // the ones who loaded it.
    if (ownsModule_ && module_ != nullptr)
    {
        FreeLibrary(static_cast<HMODULE>(module_));
    }
}

std::unique_ptr<RenderDocCapture> RenderDocCapture::TryLoad()
{
    HMODULE module = GetModuleHandleA("renderdoc.dll");
    bool ownsModule = false;

    if (module == nullptr)
    {
        module = LoadRenderDocLibrary();
        ownsModule = module != nullptr;
    }

    if (module == nullptr)
    {
        return nullptr;
    }

    const auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(
        GetProcAddress(module, "RENDERDOC_GetAPI"));

    if (getApi == nullptr)
    {
        if (ownsModule)
        {
            FreeLibrary(module);
        }

        return nullptr;
    }

    void* api = nullptr;

    if (getApi(eRENDERDOC_API_Version_1_6_0, &api) != 1 ||
        api == nullptr)
    {
        if (ownsModule)
        {
            FreeLibrary(module);
        }

        return nullptr;
    }

    return std::unique_ptr<RenderDocCapture>(
        new RenderDocCapture(module, api, ownsModule));
}

void RenderDocCapture::SetActiveWindow(
    void* const vkInstance, void* const hwnd)
{
    if (Api(api_)->SetActiveWindow != nullptr)
    {
        Api(api_)->SetActiveWindow(
            RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(vkInstance), hwnd);
    }
}

void RenderDocCapture::TriggerCapture()
{
    Api(api_)->TriggerCapture();
}

bool RenderDocCapture::IsCapturing() const
{
    return Api(api_)->IsFrameCapturing() != 0U;
}

std::string RenderDocCapture::LastCapturePath() const
{
    const u32 count = Api(api_)->GetNumCaptures();

    if (count == 0)
    {
        return {};
    }

    u32 pathLength = 0;
    Api(api_)->GetCapture(count - 1, nullptr, &pathLength, nullptr);

    if (pathLength == 0)
    {
        return {};
    }

    std::string path(pathLength, '\0');
    Api(api_)->GetCapture(count - 1, path.data(), &pathLength, nullptr);

    // GetCapture's pathlength includes the null terminator.
    if (!path.empty() && path.back() == '\0')
    {
        path.pop_back();
    }

    return path;
}
} // namespace orbit::rhi::vulkan

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <shobjidl.h>
#include <wrl/client.h>

#include <orbit/platform/FileDialog.hpp>
#include <orbit/platform/Window.hpp>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace orbit::platform
{
namespace
{
using Microsoft::WRL::ComPtr;

class ComApartment final
{
public:
    ComApartment()
    {
        const HRESULT result =
            CoInitializeEx(
                nullptr,
                COINIT_APARTMENTTHREADED);

        if (result == S_OK ||
            result == S_FALSE)
        {
            uninitialize_ = true;
            return;
        }

        if (result != RPC_E_CHANGED_MODE)
        {
            throw std::runtime_error(
                "Orbit failed to initialize COM for the native folder dialog.");
        }
    }

    ~ComApartment()
    {
        if (uninitialize_)
        {
            CoUninitialize();
        }
    }

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

private:
    bool uninitialize_{false};
};

[[nodiscard]] std::wstring Utf8ToWide(
    const std::string_view text)
{
    if (text.empty())
    {
        return {};
    }

    const int required =
        MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            text.data(),
            static_cast<int>(
                text.size()),
            nullptr,
            0);

    if (required <= 0)
    {
        throw std::runtime_error(
            "Orbit folder-dialog text is not valid UTF-8.");
    }

    std::wstring result(
        static_cast<std::size_t>(
            required),
        L'\0');

    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            text.data(),
            static_cast<int>(
                text.size()),
            result.data(),
            required) !=
        required)
    {
        throw std::runtime_error(
            "Orbit failed to convert folder-dialog text to UTF-16.");
    }

    return result;
}
} // namespace

std::optional<std::filesystem::path>
SelectFolder(
    const Window& owner,
    const FolderDialogOptions& options)
{
    ComApartment apartment;

    ComPtr<IFileDialog> dialog;

    HRESULT result =
        CoCreateInstance(
            CLSID_FileOpenDialog,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(
                &dialog));

    if (FAILED(result))
    {
        throw std::runtime_error(
            "Orbit failed to create the native folder dialog.");
    }

    FILEOPENDIALOGOPTIONS flags{};

    result =
        dialog->GetOptions(
            &flags);

    if (FAILED(result))
    {
        throw std::runtime_error(
            "Orbit failed to query native folder-dialog options.");
    }

    result =
        dialog->SetOptions(
            flags |
            FOS_PICKFOLDERS |
            FOS_FORCEFILESYSTEM |
            FOS_PATHMUSTEXIST |
            FOS_NOCHANGEDIR);

    if (FAILED(result))
    {
        throw std::runtime_error(
            "Orbit failed to configure the native folder dialog.");
    }

    if (!options.title.empty())
    {
        const std::wstring title =
            Utf8ToWide(
                options.title);

        result =
            dialog->SetTitle(
                title.c_str());

        if (FAILED(result))
        {
            throw std::runtime_error(
                "Orbit failed to set the native folder-dialog title.");
        }
    }

    if (!options.initialDirectory.empty())
    {
        const auto absolute =
            std::filesystem::weakly_canonical(
                options.initialDirectory);

        if (std::filesystem::is_directory(
                absolute))
        {
            ComPtr<IShellItem>
                initialItem;

            result =
                SHCreateItemFromParsingName(
                    absolute.c_str(),
                    nullptr,
                    IID_PPV_ARGS(
                        &initialItem));

            if (SUCCEEDED(result))
            {
                static_cast<void>(
                    dialog->SetFolder(
                        initialItem.Get()));
            }
        }
    }

    result =
        dialog->Show(
            static_cast<HWND>(
                owner.NativeHandle()));

    if (result ==
        HRESULT_FROM_WIN32(
            ERROR_CANCELLED))
    {
        return std::nullopt;
    }

    if (FAILED(result))
    {
        throw std::runtime_error(
            "Orbit native folder dialog failed.");
    }

    ComPtr<IShellItem> item;

    result =
        dialog->GetResult(
            &item);

    if (FAILED(result))
    {
        throw std::runtime_error(
            "Orbit native folder dialog returned no selection.");
    }

    PWSTR selected = nullptr;

    result =
        item->GetDisplayName(
            SIGDN_FILESYSPATH,
            &selected);

    if (FAILED(result) ||
        selected == nullptr)
    {
        throw std::runtime_error(
            "Orbit could not resolve the selected folder path.");
    }

    const std::filesystem::path path(
        selected);

    CoTaskMemFree(
        selected);

    return path;
}
} // namespace orbit::platform

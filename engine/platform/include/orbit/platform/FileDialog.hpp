#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace orbit::platform
{
class Window;

struct FolderDialogOptions
{
    std::string_view title{"Select Folder"};
    std::filesystem::path initialDirectory;
};

// Shows the native platform folder picker owned by the supplied Orbit window.
// Cancellation returns nullopt; platform/COM failures throw.
[[nodiscard]] std::optional<std::filesystem::path>
SelectFolder(
    const Window& owner,
    const FolderDialogOptions& options = {});
} // namespace orbit::platform

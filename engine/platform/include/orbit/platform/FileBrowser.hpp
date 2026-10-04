#pragma once

#include <filesystem>

namespace orbit::platform
{
// Shows a folder in the platform file browser (Explorer on Windows). A folder is
// opened; a file's folder is opened with the file selected. Throws when the path
// does not exist or the platform refuses.
void OpenInFileBrowser(const std::filesystem::path& path);
} // namespace orbit::platform

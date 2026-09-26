#include <orbit/hot_reload/ChangeClassifier.hpp>

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>

namespace orbit::hot_reload
{
namespace
{
[[nodiscard]] std::string Lower(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char ch)
        {
            return static_cast<char>(std::tolower(ch));
        });
    return value;
}

[[nodiscard]] bool IsOneOf(
    const std::string_view value,
    const std::initializer_list<std::string_view> candidates) noexcept
{
    for (const auto candidate : candidates)
    {
        if (value == candidate)
        {
            return true;
        }
    }
    return false;
}
} // namespace

ChangeKind ClassifyChange(const std::filesystem::path& path)
{
    const std::string generic =
        Lower(path.generic_string());
    const std::string filename =
        Lower(path.filename().string());
    const std::string extension =
        Lower(path.extension().string());

    if (filename == "cmakelists.txt" ||
        extension == ".cmake" ||
        filename == "moduleapi.hpp" ||
        generic.find("engine/hot_reload/include/orbit/hot_reload/moduleapi.hpp") !=
            std::string::npos ||
        generic.find("apps/editor/src/hotreloadbootstrap.cpp") !=
            std::string::npos ||
        generic.find("engine/platform/") != std::string::npos ||
        generic.find("engine/rhi/") != std::string::npos)
    {
        return ChangeKind::RestartRequired;
    }

    if (IsOneOf(
            extension,
            {".cpp", ".cc", ".cxx", ".c", ".hpp", ".hh", ".hxx", ".h", ".inl", ".ixx"}))
    {
        return ChangeKind::NativeModule;
    }

    if (IsOneOf(
            extension,
            {".hlsl", ".glsl", ".wgsl", ".vert", ".frag", ".comp", ".spv"}))
    {
        return ChangeKind::Shader;
    }

    if (IsOneOf(extension, {".lua", ".luau"}))
    {
        return ChangeKind::Script;
    }

    if (filename.ends_with(".orbitimport.toml") ||
        IsOneOf(
            extension,
            {".png", ".jpg", ".jpeg", ".tga", ".dds", ".exr", ".hdr",
             ".cube", ".fbx", ".gltf", ".glb", ".obj", ".wav", ".ogg",
             ".orbitmaterial", ".orbitdecal", ".orbitmesh", ".orbitcomponent"}))
    {
        return ChangeKind::Content;
    }

    return ChangeKind::Ignored;
}

std::string_view ChangeKindName(const ChangeKind kind) noexcept
{
    switch (kind)
    {
    case ChangeKind::NativeModule:
        return "native-module";
    case ChangeKind::Shader:
        return "shader";
    case ChangeKind::Script:
        return "script";
    case ChangeKind::Content:
        return "content";
    case ChangeKind::RestartRequired:
        return "restart-required";
    case ChangeKind::Ignored:
    default:
        return "ignored";
    }
}
} // namespace orbit::hot_reload

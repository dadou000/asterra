#include <orbit/hot_reload/ChangeClassifier.hpp>

#include <filesystem>

namespace
{
[[nodiscard]] bool Expect(
    const std::filesystem::path& path,
    const orbit::hot_reload::ChangeKind expected)
{
    return orbit::hot_reload::ClassifyChange(path) == expected;
}
} // namespace

int main()
{
    using orbit::hot_reload::ChangeKind;

    if (!Expect("engine/terrain/src/Terrain.cpp", ChangeKind::NativeModule) ||
        !Expect("Content/Shaders/terrain.hlsl", ChangeKind::Shader) ||
        !Expect("engine/render/shaders/terrain.hlsl", ChangeKind::RestartRequired) ||
        !Expect("Plugins/weather.luau", ChangeKind::Script) ||
        !Expect("Content/Materials/rock.orbitmaterial", ChangeKind::Content) ||
        !Expect("Content/Textures/rock.png", ChangeKind::Content) ||
        !Expect("engine/hot_reload/include/orbit/hot_reload/ModuleApi.hpp",
                ChangeKind::RestartRequired) ||
        !Expect("engine/rhi/vulkan/src/VulkanBackend.cpp",
                ChangeKind::RestartRequired) ||
        !Expect("README.md", ChangeKind::Ignored))
    {
        return 1;
    }

    return 0;
}

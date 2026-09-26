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
        // Shading tab: shaders hot-compile through the Shader path, shader
        // materials refresh through the Content path, neither restarts Studio.
        !Expect("Content/Shading/Lunar.shade.hlsl", ChangeKind::Shader) ||
        !Expect("Content/Shading/Lunar.orbitshadermaterial", ChangeKind::Content) ||
        !Expect("Content/Shading/Lunar.shade.hlsl.orbit-write.tmp", ChangeKind::Ignored) ||
        !Expect("Content/Textures/rock.png", ChangeKind::Content) ||
        !Expect("Content/ProjectSettings.toml", ChangeKind::Content) ||
        !Expect("engine/hot_reload/include/orbit/hot_reload/ModuleApi.hpp",
                ChangeKind::RestartRequired) ||
        !Expect("engine/rhi/vulkan/src/VulkanBackend.cpp",
                ChangeKind::RestartRequired) ||
        !Expect("apps/editor/OrbitStudio.manifest",
                ChangeKind::RestartRequired) ||
        !Expect("engine/terrain/experimental.rules",
                ChangeKind::RestartRequired) ||
        !Expect("engine/hot_reload_probe/src/sedvPxcPy", ChangeKind::Ignored) ||
        !Expect("engine/terrain/src/Terrain.cpp.swp", ChangeKind::Ignored) ||
        !Expect("engine/terrain/src/.#Terrain.cpp", ChangeKind::Ignored) ||
        !Expect("engine/terrain/src/Terrain.cpp~", ChangeKind::Ignored) ||
        !Expect("engine/terrain/src/Terrain.cpp.tmp", ChangeKind::Ignored) ||
        !Expect("engine/terrain/src/4913", ChangeKind::Ignored) ||
        !Expect("engine/terrain/src/sedimentation", ChangeKind::RestartRequired) ||
        !Expect("engine/hot_reload_probe/src/HotReloadProbe.cpp", ChangeKind::NativeModule) ||
        !Expect("README.md", ChangeKind::Ignored))
    {
        return 1;
    }

    return 0;
}

#include <orbit/content/ContentService.hpp>
#include <orbit/lighting/RuntimeMaterialEmission.hpp>

#include <filesystem>
#include <fstream>

namespace
{
void Write(
    const std::filesystem::path& path,
    const std::string_view text)
{
    std::filesystem::create_directories(
        path.parent_path());

    std::ofstream output(
        path,
        std::ios::binary |
        std::ios::trunc);

    output << text;
}
} // namespace

int main()
{
    using namespace orbit;

    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-runtime-material-emission-" +
         content::AssetId::Random().ToString());

    std::filesystem::remove_all(root);

    Write(
        root /
            "Content" /
            "Materials" /
            "panel.orbitmaterial",
        "[material]\n"
        "name = \"LED Panel\"\n"
        "emission_color_linear = [0.2, 0.6, 1.0]\n"
        "emission_luminance_nits = 1366.0\n"
        "emission_gi_enabled = true\n"
        "emission_gi_scale = 0.5\n");

    content::ContentService content(root);
    content.Scan();

    const auto materials =
        content.Search(
            "LED Panel",
            content::AssetKind::Material);

    if (materials.size() != 1U)
    {
        return 1;
    }

    const auto resolved =
        lighting::ResolveRuntimeMaterialEmission(
            content,
            materials.front().id);

    if (resolved.physical.luminanceNits !=
            1366.0F ||
        resolved.physical.colorLinear.z !=
            1.0F ||
        !resolved.physical.contributesToGi ||
        resolved.physical.giScale !=
            0.5F)
    {
        return 2;
    }

    if (resolved.evaluated.visibleRadiance.z <=
            0.0F ||
        resolved.evaluated.giRadiance.z >=
            resolved.evaluated.visibleRadiance.z)
    {
        return 3;
    }

    lighting::SurfaceData surface;

    lighting::ApplyRuntimeMaterialEmission(
        surface,
        content,
        materials.front().id.ToString());

    if (surface.emissionRadianceSceneLinear.z <=
            0.0F ||
        surface.emissionGiScale != 0.5F)
    {
        return 4;
    }

    std::filesystem::remove_all(root);
    return 0;
}

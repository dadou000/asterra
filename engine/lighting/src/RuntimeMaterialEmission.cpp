#include <orbit/lighting/RuntimeMaterialEmission.hpp>

#include <stdexcept>

namespace orbit::lighting
{
RuntimeMaterialEmission
ResolveRuntimeMaterialEmission(
    const content::ContentService& contentService,
    const content::AssetId asset)
{
    const auto* record =
        contentService.Find(asset);

    if (record == nullptr ||
        (record->kind !=
             content::AssetKind::Material &&
         record->kind !=
             content::AssetKind::MaterialInstance))
    {
        throw std::invalid_argument(
            "Runtime material emission requires a Material or Material Instance asset.");
    }

    const auto source =
        contentService.ResolveMaterialEmission(
            asset);

    PhysicalMaterialEmission physical{
        .colorLinear = {
            static_cast<f32>(
                source.colorLinear[0]),
            static_cast<f32>(
                source.colorLinear[1]),
            static_cast<f32>(
                source.colorLinear[2])
        },
        .luminanceNits =
            static_cast<f32>(
                source.luminanceNits),
        .contributesToGi =
            source.contributesToGi,
        .giScale =
            static_cast<f32>(
                source.giScale)
    };

    return {
        .asset = asset,
        .physical = physical,
        .evaluated =
            EvaluateMaterialEmission(
                physical)
    };
}

RuntimeMaterialEmission
ResolveRuntimeMaterialEmission(
    const content::ContentService& contentService,
    const std::string_view assetIdText)
{
    const auto parsed =
        content::AssetId::Parse(
            assetIdText);

    if (!parsed.has_value())
    {
        throw std::invalid_argument(
            "Material Assignment contains an invalid AssetId.");
    }

    return
        ResolveRuntimeMaterialEmission(
            contentService,
            *parsed);
}

void ApplyRuntimeMaterialEmission(
    SurfaceData& surface,
    const content::ContentService& contentService,
    const content::AssetId asset)
{
    const auto resolved =
        ResolveRuntimeMaterialEmission(
            contentService,
            asset);

    ApplyMaterialEmission(
        surface,
        resolved.physical);
}

void ApplyRuntimeMaterialEmission(
    SurfaceData& surface,
    const content::ContentService& contentService,
    const std::string_view assetIdText)
{
    const auto resolved =
        ResolveRuntimeMaterialEmission(
            contentService,
            assetIdText);

    ApplyMaterialEmission(
        surface,
        resolved.physical);
}
} // namespace orbit::lighting

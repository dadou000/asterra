#pragma once

#include <orbit/content/ContentService.hpp>
#include <orbit/lighting/MaterialEmission.hpp>

#include <string_view>

namespace orbit::lighting
{
struct RuntimeMaterialEmission
{
    content::AssetId asset{};
    PhysicalMaterialEmission physical{};
    EvaluatedMaterialEmission evaluated{};
};

[[nodiscard]] RuntimeMaterialEmission
ResolveRuntimeMaterialEmission(
    const content::ContentService& content,
    content::AssetId asset);

[[nodiscard]] RuntimeMaterialEmission
ResolveRuntimeMaterialEmission(
    const content::ContentService& content,
    std::string_view assetIdText);

void ApplyRuntimeMaterialEmission(
    SurfaceData& surface,
    const content::ContentService& content,
    content::AssetId asset);

void ApplyRuntimeMaterialEmission(
    SurfaceData& surface,
    const content::ContentService& content,
    std::string_view assetIdText);
} // namespace orbit::lighting

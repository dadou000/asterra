#include <orbit/lighting/RuntimeEmissiveSurface.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>

namespace orbit::lighting
{
namespace
{
[[nodiscard]] f32 DecodeChannel(
    const u8 value,
    const EmissiveTextureTransfer transfer) noexcept
{
    const f32 encoded =
        static_cast<f32>(value) /
        255.0F;

    if (transfer ==
        EmissiveTextureTransfer::Linear)
    {
        return encoded;
    }

    return encoded <= 0.04045F
        ? encoded / 12.92F
        : std::pow(
              (encoded + 0.055F) /
                  1.055F,
              2.4F);
}

[[nodiscard]] std::filesystem::path
ResolveEmissiveTexturePath(
    const content::ContentService& content,
    const content::AssetRecord& record,
    u32 depth = 0U)
{
    if (depth > 32U)
    {
        throw std::runtime_error(
            "Material inheritance is cyclic or exceeds the supported depth.");
    }

    if (record.kind ==
            content::AssetKind::Material &&
        record.material.has_value())
    {
        return
            record.material->emissive;
    }

    if (record.kind ==
            content::AssetKind::MaterialInstance &&
        record.materialInstance.has_value())
    {
        const auto* parent =
            content.FindByPath(
                record.materialInstance->
                    parent);

        if (parent == nullptr)
        {
            throw std::runtime_error(
                "Material instance parent could not be resolved while building emissive hierarchy.");
        }

        return
            ResolveEmissiveTexturePath(
                content,
                *parent,
                depth + 1U);
    }

    return {};
}

[[nodiscard]] std::vector<std::byte>
ReadBytes(
    const std::filesystem::path& path)
{
    std::ifstream stream(
        path,
        std::ios::binary |
            std::ios::ate);

    if (!stream)
    {
        throw std::runtime_error(
            "Cannot open runtime emissive texture artifact: " +
            path.string());
    }

    const auto end =
        stream.tellg();

    if (end < 0)
    {
        throw std::runtime_error(
            "Cannot determine runtime emissive texture size.");
    }

    std::vector<std::byte> bytes(
        static_cast<std::size_t>(
            end));

    stream.seekg(0);

    if (!bytes.empty())
    {
        stream.read(
            reinterpret_cast<char*>(
                bytes.data()),
            static_cast<std::streamsize>(
                bytes.size()));
    }

    if (!stream)
    {
        throw std::runtime_error(
            "Cannot read runtime emissive texture artifact.");
    }

    return bytes;
}
} // namespace

EmissiveSurfaceGrid
RuntimeEmissiveSurface::Grid() const noexcept
{
    return {
        .frame = geometry.frame,
        .body = geometry.body,
        .stableId = geometry.stableId,
        .originInFrameMeters =
            geometry.originInFrameMeters,
        .axisUInFrameMeters =
            geometry.axisUInFrameMeters,
        .axisVInFrameMeters =
            geometry.axisVInFrameMeters,
        .width = width,
        .height = height,
        .giRadiance =
            std::span<const math::Float3>(
                giRadiance.data(),
                giRadiance.size())
    };
}

RuntimeEmissiveSurface
BuildRuntimeEmissiveSurface(
    const RuntimeEmissiveSurfaceGeometry& geometry,
    const EvaluatedMaterialEmission& emission,
    const content::RuntimeTexture* emissiveTexture,
    const EmissiveTextureTransfer transfer)
{
    if (!geometry.frame ||
        !geometry.body ||
        geometry.stableId == 0U)
    {
        throw std::invalid_argument(
            "Runtime emissive surface requires valid frame/body/stable ID.");
    }

    RuntimeEmissiveSurface result{
        .geometry = geometry
    };

    if (emissiveTexture == nullptr)
    {
        result.width = 1U;
        result.height = 1U;
        result.giRadiance.push_back(
            emission.giRadiance);
        return result;
    }

    if (emissiveTexture->format !=
            content::RuntimeTextureFormat::
                Rgba8Unorm ||
        emissiveTexture->width == 0U ||
        emissiveTexture->height == 0U ||
        emissiveTexture->pixels.size() !=
            static_cast<std::size_t>(
                emissiveTexture->width) *
            emissiveTexture->height *
            4U)
    {
        throw std::invalid_argument(
            "Runtime emissive hierarchy currently requires a valid RGBA8 runtime texture.");
    }

    result.width =
        emissiveTexture->width;
    result.height =
        emissiveTexture->height;
    result.giRadiance.resize(
        static_cast<std::size_t>(
            result.width) *
        result.height);

    for (std::size_t index = 0U;
         index < result.giRadiance.size();
         ++index)
    {
        const std::size_t offset =
            index * 4U;

        const f32 r =
            DecodeChannel(
                std::to_integer<u8>(
                    emissiveTexture->
                        pixels[offset + 0U]),
                transfer);
        const f32 g =
            DecodeChannel(
                std::to_integer<u8>(
                    emissiveTexture->
                        pixels[offset + 1U]),
                transfer);
        const f32 b =
            DecodeChannel(
                std::to_integer<u8>(
                    emissiveTexture->
                        pixels[offset + 2U]),
                transfer);

        result.giRadiance[index] = {
            emission.giRadiance.x * r,
            emission.giRadiance.y * g,
            emission.giRadiance.z * b
        };
    }

    return result;
}

RuntimeEmissiveSurface
BuildRuntimeEmissiveSurface(
    content::ContentService& contentService,
    const content::AssetId materialAsset,
    const RuntimeEmissiveSurfaceGeometry& geometry,
    const EmissiveTextureTransfer transfer)
{
    const auto* record =
        contentService.Find(
            materialAsset);

    if (record == nullptr)
    {
        throw std::invalid_argument(
            "Runtime emissive surface references an unknown material asset.");
    }

    const auto materialEmission =
        ResolveRuntimeMaterialEmission(
            contentService,
            materialAsset);

    const auto emissivePath =
        ResolveEmissiveTexturePath(
            contentService,
            *record);

    if (emissivePath.empty())
    {
        return
            BuildRuntimeEmissiveSurface(
                geometry,
                materialEmission.evaluated,
                nullptr,
                transfer);
    }

    const auto* textureAsset =
        contentService.FindByPath(
            emissivePath);

    if (textureAsset == nullptr ||
        textureAsset->kind !=
            content::AssetKind::Texture)
    {
        throw std::runtime_error(
            "Material emissive texture is not indexed as a texture asset.");
    }

    const auto imported =
        contentService.ImportDerived(
            textureAsset->id);

    const auto artifact =
        std::find_if(
            imported.artifacts.begin(),
            imported.artifacts.end(),
            [](const content::ImportedArtifact& item)
            {
                return
                    item.name ==
                    "texture.orbittex";
            });

    if (artifact ==
        imported.artifacts.end())
    {
        throw std::runtime_error(
            "Texture importer did not produce texture.orbittex for emissive hierarchy.");
    }

    const auto bytes =
        ReadBytes(
            artifact->path);

    const auto texture =
        content::DecodeRuntimeTexture(
            bytes);

    return
        BuildRuntimeEmissiveSurface(
            geometry,
            materialEmission.evaluated,
            &texture,
            transfer);
}
} // namespace orbit::lighting

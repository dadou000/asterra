from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
PATH = ROOT / "apps/editor/src/Main.cpp"


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one match, found {count}")
    return text.replace(old, new, 1)


def main() -> None:
    text = PATH.read_text(encoding="utf-8")

    text = replace_once(
        text,
        "#include <orbit/content/ContentService.hpp>\n",
        "#include <orbit/content/ContentService.hpp>\n#include <orbit/content/RuntimeTexture.hpp>\n",
        "runtime texture include",
    )

    helper_marker = '''[[nodiscard]] std::filesystem::path
FindPlayerExecutable()
'''
    helper = r'''[[nodiscard]] orbit::math::Float3
AverageTextureColor(
    orbit::content::ContentService& content,
    const orbit::content::AssetRecord* texture)
{
    constexpr orbit::math::Float3 fallback{
        0.34F,
        0.37F,
        0.42F
    };

    if (texture == nullptr ||
        texture->kind != orbit::content::AssetKind::Texture ||
        !texture->derivedKey.has_value())
    {
        return fallback;
    }

    const auto bytes =
        content.Cache().Read(
            *texture->derivedKey,
            "texture.orbittex");

    if (!bytes.has_value())
    {
        return fallback;
    }

    try
    {
        const auto runtimeTexture =
            orbit::content::DecodeRuntimeTexture(
                std::span(
                    bytes->data(),
                    bytes->size()));

        const std::size_t pixelCount =
            runtimeTexture.pixels.size() / 4U;

        if (pixelCount == 0)
        {
            return fallback;
        }

        const std::size_t stride =
            std::max<std::size_t>(
                1U,
                pixelCount / 4096U);

        orbit::f64 red = 0.0;
        orbit::f64 green = 0.0;
        orbit::f64 blue = 0.0;
        std::size_t samples = 0;

        for (std::size_t pixel = 0;
             pixel < pixelCount;
             pixel += stride)
        {
            const std::size_t offset =
                pixel * 4U;

            red += std::to_integer<orbit::u8>(
                runtimeTexture.pixels[offset]);
            green += std::to_integer<orbit::u8>(
                runtimeTexture.pixels[offset + 1U]);
            blue += std::to_integer<orbit::u8>(
                runtimeTexture.pixels[offset + 2U]);
            ++samples;
        }

        const orbit::f32 scale =
            1.0F /
            static_cast<orbit::f32>(
                samples * 255U);

        return {
            static_cast<orbit::f32>(red) * scale,
            static_cast<orbit::f32>(green) * scale,
            static_cast<orbit::f32>(blue) * scale
        };
    }
    catch (const std::exception&)
    {
        return fallback;
    }
}

[[nodiscard]] orbit::editor_ui::PreviewMaterial
PreviewMaterialForAsset(
    orbit::content::ContentService& content,
    const orbit::content::AssetRecord* asset,
    const orbit::u32 depth = 0)
{
    orbit::editor_ui::PreviewMaterial result{};

    if (asset == nullptr || depth > 4U)
    {
        return result;
    }

    if (asset->kind == orbit::content::AssetKind::Material &&
        asset->material.has_value())
    {
        const auto& material = *asset->material;
        result.roughness = static_cast<orbit::f32>(
            std::clamp(material.roughnessFactor, 0.0, 1.0));
        result.metallic = static_cast<orbit::f32>(
            std::clamp(material.metallicFactor, 0.0, 1.0));

        if (!material.baseColor.empty())
        {
            const auto texturePath =
                asset->sourcePath.parent_path() /
                material.baseColor;
            result.baseColor =
                AverageTextureColor(
                    content,
                    content.FindByPath(texturePath));
        }

        return result;
    }

    if (asset->kind == orbit::content::AssetKind::MaterialInstance &&
        asset->materialInstance.has_value())
    {
        const auto& instance =
            *asset->materialInstance;
        const auto parentPath =
            asset->sourcePath.parent_path() /
            instance.parent;

        result = PreviewMaterialForAsset(
            content,
            content.FindByPath(parentPath),
            depth + 1U);

        if (instance.roughnessFactor.has_value())
        {
            result.roughness = static_cast<orbit::f32>(
                std::clamp(
                    *instance.roughnessFactor,
                    0.0,
                    1.0));
        }

        if (instance.metallicFactor.has_value())
        {
            result.metallic = static_cast<orbit::f32>(
                std::clamp(
                    *instance.metallicFactor,
                    0.0,
                    1.0));
        }

        return result;
    }

    if (asset->kind == orbit::content::AssetKind::Decal &&
        asset->decal.has_value())
    {
        const auto texturePath =
            asset->sourcePath.parent_path() /
            asset->decal->texture;
        result.baseColor =
            AverageTextureColor(
                content,
                content.FindByPath(texturePath));
        result.roughness = 0.55F;
        result.metallic = 0.0F;
    }

    return result;
}

'''
    text = replace_once(
        text,
        helper_marker,
        helper + helper_marker,
        "preview material helper",
    )

    register_block = '''        orbit::editor_model::
            authoring_commands::Register(
                authoringCommands,
                commandService,
                objects,
                selection);

        orbit::editor_model::
            CommandSurfaceRegistry
'''
    register_new = '''        orbit::editor_model::
            authoring_commands::Register(
                authoringCommands,
                commandService,
                objects,
                selection);

        orbit::editor_model::
            authoring_commands::RegisterMaterialCommands(
                authoringCommands,
                commandService,
                objects,
                selection);

        orbit::editor_model::
            CommandSurfaceRegistry
'''
    text = replace_once(
        text,
        register_block,
        register_new,
        "material command registration",
    )

    body_view = '''        orbit::render_view::RenderView
            bodyView(
                device,
                {
                    .width = 960,
                    .height = 640
                });
'''
    body_view_new = body_view + '''
        orbit::render_view::RenderView
            materialView(
                device,
                {
                    .width = 384,
                    .height = 240
                });
'''
    text = replace_once(
        text,
        body_view,
        body_view_new,
        "second render view",
    )

    camera_block = '''        bodyView.Camera().up = {
            0.0F,
            1.0F,
            0.0F
        };

        editorRpc.AttachViewport({
'''
    camera_new = '''        bodyView.Camera().up = {
            0.0F,
            1.0F,
            0.0F
        };

        materialView.Camera().frame =
            bodyView.Camera().frame;
        materialView.Camera().localPositionMeters = {
            0.0,
            0.0,
            -3.2
        };
        materialView.Camera().nearPlaneMeters = 0.01F;
        materialView.Camera().farPlaneMeters = 10.0F;
        materialView.Camera().forward = {
            0.0F,
            0.0F,
            1.0F
        };
        materialView.Camera().up = {
            0.0F,
            1.0F,
            0.0F
        };

        editorRpc.AttachViewport({
'''
    text = replace_once(
        text,
        camera_block,
        camera_new,
        "material preview camera",
    )

    state_block = '''        std::string explorerSearch;
        std::string contentSearch;
        std::string renameBuffer;
'''
    state_new = '''        std::string explorerSearch;
        std::string contentSearch;
        std::optional<orbit::content::AssetId>
            materialPreviewAsset;
        orbit::editor_ui::PreviewMaterial
            materialPreviewMaterial{};
        std::string renameBuffer;
'''
    text = replace_once(
        text,
        state_block,
        state_new,
        "material preview state",
    )

    content_capture = '''            .draw =
                [&content,
                 &contentSearch,
                 &window](
'''
    content_capture_new = '''            .draw =
                [&content,
                 &contentSearch,
                 &materialView,
                 &materialPreviewAsset,
                 &materialPreviewMaterial,
                 &window](
'''
    text = replace_once(
        text,
        content_capture,
        content_capture_new,
        "material panel capture",
    )

    assets_marker = '''                    context.Separator();

                    const auto assets =
                        content.Search(
                            contentSearch);
'''
    assets_new = '''                    context.Separator();

                    const auto previewAvailable =
                        context.ContentAvailable();
                    const orbit::f32 previewWidth =
                        std::clamp(
                            previewAvailable.width,
                            180.0F,
                            520.0F);
                    const orbit::f32 previewHeight =
                        previewWidth * 0.625F;

                    const orbit::u32 previewPixelsWide =
                        static_cast<orbit::u32>(
                            std::max(previewWidth, 1.0F));
                    const orbit::u32 previewPixelsHigh =
                        static_cast<orbit::u32>(
                            std::max(previewHeight, 1.0F));

                    if (materialView.Width() != previewPixelsWide ||
                        materialView.Height() != previewPixelsHigh)
                    {
                        materialView.Resize(
                            previewPixelsWide,
                            previewPixelsHigh);
                    }

                    context.Text(
                        materialPreviewAsset.has_value()
                            ? "Rendered material/decal preview"
                            : "Select a material, instance or decal to preview");
                    static_cast<void>(
                        context.Image(
                            materialView.Color(),
                            {
                                .width = previewWidth,
                                .height = previewHeight
                            }));
                    context.Separator();

                    const auto assets =
                        content.Search(
                            contentSearch);
'''
    text = replace_once(
        text,
        assets_marker,
        assets_new,
        "material preview image",
    )

    selectable = '''                        static_cast<void>(
                            context.Selectable(
                                label,
                                false));

                        if (asset.kind ==
'''
    selectable_new = '''                        const bool previewSelected =
                            materialPreviewAsset.has_value() &&
                            *materialPreviewAsset == asset.id;

                        if (context.Selectable(
                                label,
                                previewSelected))
                        {
                            if (asset.kind ==
                                    orbit::content::AssetKind::Material ||
                                asset.kind ==
                                    orbit::content::AssetKind::MaterialInstance ||
                                asset.kind ==
                                    orbit::content::AssetKind::Decal)
                            {
                                materialPreviewAsset = asset.id;
                                materialPreviewMaterial =
                                    PreviewMaterialForAsset(
                                        content,
                                        &asset);
                            }
                        }

                        if (asset.kind ==
'''
    text = replace_once(
        text,
        selectable,
        selectable_new,
        "material asset selection",
    )

    drag_marker = '''                        if (context.
                                BeginDragSource())
'''
    decal_button = '''                        if (asset.kind ==
                            orbit::content::AssetKind::Texture)
                        {
                            context.SameLine();

                            const std::string decalLabel =
                                "Create Decal##asset-" +
                                asset.id.ToString();

                            if (context.Button(decalLabel))
                            {
                                try
                                {
                                    const auto decalId =
                                        content.CreateDecal(
                                            asset.id);
                                    const auto* decal =
                                        content.Find(decalId);
                                    materialPreviewAsset = decalId;
                                    materialPreviewMaterial =
                                        PreviewMaterialForAsset(
                                            content,
                                            decal);

                                    orbit::log::Info(
                                        std::format(
                                            "Created decal '{}'.",
                                            decal != nullptr
                                                ? decal->name
                                                : decalId.ToString()));
                                }
                                catch (const std::exception& exception)
                                {
                                    orbit::log::Warning(
                                        std::format(
                                            "Decal creation failed: {}",
                                            exception.what()));
                                }
                            }
                        }

'''
    text = replace_once(
        text,
        drag_marker,
        decal_button + drag_marker,
        "texture decal creation",
    )

    # Extend viewport drop without duplicating the material path.
    warning = '''                            else
                            {
                                orbit::log::Warning(
                                    "Viewport drop expects a material asset.");
                            }
'''
    decal_drop = '''                            else if (
                                asset != nullptr &&
                                asset->kind ==
                                    orbit::content::AssetKind::Decal &&
                                asset->decal.has_value())
                            {
                                try
                                {
                                    const auto* body =
                                        bodies.FindBody(bodyId);
                                    const auto ray =
                                        orbit::render_view::ViewportRay(
                                            bodyView.Camera(),
                                            bodyView.Width(),
                                            bodyView.Height(),
                                            interaction.u,
                                            interaction.v);

                                    if (body == nullptr || !ray.has_value())
                                    {
                                        throw std::runtime_error(
                                            "Decal drop could not construct a body-local view ray.");
                                    }

                                    const auto hit =
                                        orbit::universe::IntersectReferenceSurfaceRay(
                                            body->shape,
                                            ray->origin,
                                            ray->direction);

                                    if (!hit.has_value())
                                    {
                                        throw std::runtime_error(
                                            "Decal drop did not hit the active body.");
                                    }

                                    const auto coordinate =
                                        orbit::universe::ReferenceSurfaceCoordinate(
                                            body->shape,
                                            *hit);

                                    if (!coordinate.has_value())
                                    {
                                        throw std::runtime_error(
                                            "Decal drop could not resolve a surface coordinate.");
                                    }

                                    const std::array selected{
                                        bodyObject
                                    };
                                    selection.Set(std::span(selected));

                                    authoringCommands.Invoke(
                                        orbit::editor_model::authoring_commands::kAttachDecal,
                                        {
                                            {"decal", asset->sourcePath.generic_string()},
                                            {"latitude", coordinate->latitudeRadians},
                                            {"longitude", coordinate->longitudeRadians},
                                            {"width", asset->decal->widthMeters},
                                            {"height", asset->decal->heightMeters},
                                            {"rotation", 0.0},
                                            {"opacity", asset->decal->opacity}
                                        });
                                }
                                catch (const std::exception& exception)
                                {
                                    orbit::log::Warning(exception.what());
                                }
                            }
                            else
                            {
                                orbit::log::Warning(
                                    "Viewport drop expects a material or decal asset.");
                            }
'''
    text = replace_once(
        text,
        warning,
        decal_drop,
        "viewport decal drop",
    )

    import_marker = '''            const auto viewTargets =
                bodyView.Import(
                    graph,
                    "StudioBody");

            const auto backBufferTarget =
'''
    import_new = '''            const auto viewTargets =
                bodyView.Import(
                    graph,
                    "StudioBody");

            const auto materialViewTargets =
                materialView.Import(
                    graph,
                    "StudioMaterialPreview");

            const auto backBufferTarget =
'''
    text = replace_once(
        text,
        import_marker,
        import_new,
        "material render view import",
    )

    paths_pass = '''            graph.AddPass(
                "Studio.Paths",
'''
    material_pass = '''            graph.AddPass(
                "Studio.MaterialPreview",
                {
                    {
                        .texture =
                            materialViewTargets.color,
                        .state =
                            orbit::rhi::ResourceState::RenderTarget,
                        .access =
                            orbit::render_graph::Access::Write
                    }
                },
                [&](orbit::rhi::CommandList& commandList,
                    const orbit::render_graph::Resources&)
                {
                    bodyPreview.Draw(
                        commandList,
                        materialView.Color(),
                        materialView.Width(),
                        materialView.Height(),
                        orbit::universe::BodyShape{
                            orbit::universe::SphereShape{
                                .radiusMeters = 1.0
                            }
                        },
                        materialView.Camera(),
                        materialPreviewMaterial);
                });

'''
    text = replace_once(
        text,
        paths_pass,
        material_pass + paths_pass,
        "material preview render pass",
    )

    ui_resource = '''                    {
                        .texture =
                            backBufferTarget,
                        .state =
                            orbit::rhi::
                                ResourceState::
                                    RenderTarget,
                        .access =
                            orbit::render_graph::
                                Access::Write
                    }
'''
    ui_resource_new = '''                    {
                        .texture =
                            materialViewTargets.color,
                        .state =
                            orbit::rhi::
                                ResourceState::
                                    ShaderResource,
                        .access =
                            orbit::render_graph::
                                Access::Read
                    },
                    {
                        .texture =
                            backBufferTarget,
                        .state =
                            orbit::rhi::
                                ResourceState::
                                    RenderTarget,
                        .access =
                            orbit::render_graph::
                                Access::Write
                    }
'''
    # This render-target resource shape appears in Canvas too. Restrict to the
    # Studio.Ui block by slicing from its marker.
    ui_index = text.index('            graph.AddPass(\n                "Studio.Ui",')
    prefix = text[:ui_index]
    suffix = text[ui_index:]
    suffix = replace_once(
        suffix,
        ui_resource,
        ui_resource_new,
        "material preview UI resource",
    )
    text = prefix + suffix

    PATH.write_text(text, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()

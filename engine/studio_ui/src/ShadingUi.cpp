#include <orbit/studio_ui/ShadingUi.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <numbers>
#include <span>
#include <vector>

namespace orbit::studio_ui
{
namespace
{
constexpr std::string_view kEntryPayload = "ORBIT_SHADING_ENTRY";

[[nodiscard]] std::vector<std::string_view> Labels(
    const std::span<const shading::EnumEntry> table)
{
    std::vector<std::string_view> result;
    for (const auto& entry : table)
    {
        result.push_back(entry.label);
    }
    return result;
}

[[nodiscard]] std::string KindLabel(const shading::TreeNode& node)
{
    return node.kind.has_value()
        ? std::string(content::AssetKindName(*node.kind))
        : std::string();
}

[[nodiscard]] std::vector<std::byte> Bytes(const std::string& text)
{
    const auto view = std::as_bytes(std::span(text));
    return {view.begin(), view.end()};
}
} // namespace

ShadingUi::ShadingUi(
    shading::ShadingWorkspace& workspace,
    std::function<rhi::Texture*()> previewColor)
    : workspace_(&workspace),
      previewColor_(std::move(previewColor))
{
}

void ShadingUi::Register(editor_ui::EditorUi& ui)
{
    ui.RegisterPanel({
        .id = kPanelId,
        .title = "Shading",
        .defaultOpen = true,
        .defaultDock = editor_ui::DockRegion::Center,
        .dockOrder = 25,
        .minSize = {.width = 640.0F, .height = 420.0F},
        .defaultSize = {.width = 1280.0F, .height = 900.0F},
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                Draw(context);
            }});
}

std::optional<std::pair<u32, u32>> ShadingUi::TakePreviewResizeRequest()
{
    auto request = pendingResize_;
    pendingResize_.reset();
    return request;
}

void ShadingUi::Run(
    const std::string& success,
    const std::function<void()>& action)
{
    try
    {
        action();
        status_ = success;
        statusIsError_ = false;
    }
    catch (const std::exception& exception)
    {
        status_ = std::string("Failed: ") + exception.what();
        statusIsError_ = true;
    }
}

std::string ShadingUi::TargetFolder() const
{
    const auto& selected = workspace_->Selected();

    if (selected.empty())
    {
        return "Content";
    }

    const auto* record = workspace_->Content().FindByPath(selected);
    return record == nullptr
        ? selected.generic_string()
        : selected.parent_path().generic_string();
}

void ShadingUi::Draw(editor_ui::PanelContext& context)
{
    const auto available = context.ContentAvailable();
    constexpr f32 kTreeWidth = 300.0F;

    if (context.BeginChild(
            "##shading-tree",
            {.width = kTreeWidth, .height = available.height},
            true))
    {
        DrawTree(context);
    }
    context.EndChild();

    context.SameLine();

    if (context.BeginChild(
            "##shading-work",
            {.width = std::max(available.width - kTreeWidth - 12.0F, 100.0F),
             .height = available.height},
            false))
    {
        DrawWorkArea(context);
    }
    context.EndChild();
}

// ---------------------------------------------------------------------------
// Content tree
// ---------------------------------------------------------------------------

void ShadingUi::DrawTree(editor_ui::PanelContext& context)
{
    context.Heading("Material Service");

    const std::string folder = TargetFolder();
    context.MutedText("New items go in: " + folder);

    static_cast<void>(context.InputText("Name##shading-name", nameBuffer_));

    static const std::vector<std::string_view> templates = [] {
        std::vector<std::string_view> names;
        for (const auto name : shading::ShaderTemplateNames())
        {
            names.push_back(name);
        }
        return names;
    }();
    static_cast<void>(context.Combo("Template##shading-template", templates, templateIndex_));

    if (context.Button("New Folder"))
    {
        Run("Folder created.",
            [&] { workspace_->CreateFolder(std::filesystem::path(folder) / nameBuffer_); });
    }
    context.SameLine();
    if (context.Button("New Shader"))
    {
        Run("Shader created.",
            [&]
            {
                const auto path = workspace_->CreateShader(
                    folder, nameBuffer_,
                    templates[static_cast<std::size_t>(templateIndex_)]);
                workspace_->Select(path);
            });
    }

    const auto& status = workspace_->Status();
    const bool canMakeMaterial = !status.shader.empty();

    if (context.Button("New Material"))
    {
        if (!canMakeMaterial)
        {
            status_ = "Failed: select a shader (or a material) first.";
            statusIsError_ = true;
        }
        else
        {
            Run("Shader material created.",
                [&]
                {
                    const auto path = workspace_->CreateShaderMaterial(
                        folder, nameBuffer_, status.shader);
                    workspace_->Select(path);
                });
        }
    }
    context.SameLine();
    if (context.Button("Rename"))
    {
        Run("Renamed.",
            [&]
            {
                if (workspace_->Selected().empty())
                {
                    throw std::invalid_argument("Nothing is selected.");
                }
                workspace_->Rename(workspace_->Selected(), nameBuffer_);
            });
    }
    context.SameLine();
    if (context.Button("Trash"))
    {
        Run("Moved to .orbit/Trash (recoverable).",
            [&]
            {
                if (workspace_->Selected().empty())
                {
                    throw std::invalid_argument("Nothing is selected.");
                }
                static_cast<void>(workspace_->Trash(workspace_->Selected()));
            });
    }

    if (!status_.empty())
    {
        if (statusIsError_)
        {
            context.ErrorText(status_);
        }
        else
        {
            context.MutedText(status_);
        }
    }

    context.Separator();

    // The Content root is a fixed header (selecting it targets top-level
    // creation) with its children always listed beneath it.
    const auto root = workspace_->Tree();
    if (context.Selectable("Content##shading-root", workspace_->Selected() == root.path))
    {
        Run({}, [&] { workspace_->Select(root.path); });
        status_.clear();
    }
    if (const auto payload = context.AcceptDragPayload(kEntryPayload))
    {
        const std::string source(
            reinterpret_cast<const char*>(payload->data()), payload->size());
        Run("Moved to the Content root.", [&] { workspace_->Move(source, root.path); });
    }
    for (const auto& child : root.children)
    {
        DrawTreeNode(context, child);
    }
}

void ShadingUi::DrawTreeNode(
    editor_ui::PanelContext& context,
    const shading::TreeNode& node)
{
    const std::string id = "##shading-" + node.path.generic_string();
    const bool selected = workspace_->Selected() == node.path;

    if (node.folder)
    {
        const auto item = context.TreeItem(node.name + id, selected);

        if (item.clicked)
        {
            Run({}, [&] { workspace_->Select(node.path); });
            status_.clear();
        }

        // Drag a file or folder onto a folder to move it there.
        if (node.path != "Content" && context.BeginDragSource())
        {
            context.SetDragPayload(kEntryPayload, Bytes(node.path.generic_string()));
            context.EndDragSource();
        }
        if (const auto payload = context.AcceptDragPayload(kEntryPayload))
        {
            const std::string source(
                reinterpret_cast<const char*>(payload->data()), payload->size());
            Run("Moved.", [&] { workspace_->Move(source, node.path); });
        }

        if (node.path != "Content")
        {
            const std::vector<editor_ui::ActionPresentation> actions{
                {.label = "New Folder Here",
                 .invoke = [this, path = node.path]
                 {
                     Run("Folder created.",
                         [&] { workspace_->CreateFolder(path / nameBuffer_); });
                 }},
                {.label = "New Shader Here",
                 .invoke = [this, path = node.path]
                 {
                     Run("Shader created.",
                         [&]
                         {
                             const auto created = workspace_->CreateShader(
                                 path, nameBuffer_, "lit");
                             workspace_->Select(created);
                         });
                 }},
                {.label = "Move to Trash",
                 .invoke = [this, path = node.path]
                 {
                     Run("Moved to .orbit/Trash (recoverable).",
                         [&] { static_cast<void>(workspace_->Trash(path)); });
                 }}};
            context.ContextMenu("##ctx-" + node.path.generic_string(), actions, item.rightClicked);
        }

        if (item.open)
        {
            for (const auto& child : node.children)
            {
                DrawTreeNode(context, child);
            }
            context.TreePop();
        }
        return;
    }

    std::string label = node.name;
    if (const auto kind = KindLabel(node); !kind.empty())
    {
        label += "   (" + kind + ")";
    }

    // A mesh is the preview subject rather than the edited asset, so it is
    // highlighted when it is the current preview mesh.
    const bool isPreviewMesh = node.kind == content::AssetKind::Mesh &&
        workspace_->Preview().mesh == node.path.generic_string();

    if (context.Selectable(label + id, selected || isPreviewMesh))
    {
        Run({}, [&] { workspace_->Select(node.path); });
        status_.clear();
    }

    if (context.BeginDragSource())
    {
        context.SetDragPayload(kEntryPayload, Bytes(node.path.generic_string()));
        context.EndDragSource();
    }

    const std::vector<editor_ui::ActionPresentation> actions{
        {.label = "Move to Trash",
         .invoke = [this, path = node.path]
         {
             Run("Moved to .orbit/Trash (recoverable).",
                 [&] { static_cast<void>(workspace_->Trash(path)); });
         }}};
    context.ContextMenu("##ctx-" + node.path.generic_string(), actions, false);
}

// ---------------------------------------------------------------------------
// Work area
// ---------------------------------------------------------------------------

void ShadingUi::DrawWorkArea(editor_ui::PanelContext& context)
{
    const auto& status = workspace_->Status();

    if (status.shader.empty())
    {
        context.Heading("Shading");
        context.Text(
            "Select a shader (*.shade.hlsl) or a shader material in the tree, "
            "or create one with New Shader.");
        context.MutedText(
            "Edits recompile live and are swapped into the preview with no "
            "restart. Files saved by an external editor reload automatically.");
        return;
    }

    context.Heading(
        status.material.empty() ? status.shader.filename().string()
                                : status.material.filename().string());
    context.MutedText(
        status.material.empty()
            ? status.shader.generic_string()
            : status.shader.generic_string() + "  (via material)");

    if (context.Section("Preview", true))
    {
        DrawPreview(context);
    }

    if (context.Section("Parameters", true))
    {
        DrawParameters(context);
    }

    if (context.Section("Shader source", true))
    {
        DrawEditor(context, 320.0F);
    }
}

void ShadingUi::DrawPreview(editor_ui::PanelContext& context)
{
    auto& preview = workspace_->Preview();
    const auto available = context.ContentAvailable();

    const f32 width = std::clamp(available.width, 240.0F, 820.0F);
    const f32 height = width * 0.6F;

    const std::pair<u32, u32> wanted{
        static_cast<u32>(std::max(width, 1.0F)),
        static_cast<u32>(std::max(height, 1.0F))};
    if (wanted != lastRequestedSize_)
    {
        lastRequestedSize_ = wanted;
        pendingResize_ = wanted;
    }

    if (rhi::Texture* const color = previewColor_ ? previewColor_() : nullptr)
    {
        const auto view = context.InteractiveImage(
            "##shading-preview", *color, {.width = width, .height = height});

        if (view.dragging)
        {
            preview.camera.yawRadians -= view.dragDeltaX * 0.01F;
            preview.camera.pitchRadians = std::clamp(
                preview.camera.pitchRadians + view.dragDeltaY * 0.01F, -1.45F, 1.45F);
        }
        if (view.hovered && view.wheel != 0.0F)
        {
            preview.camera.distance = std::clamp(
                preview.camera.distance * (1.0F - view.wheel * 0.1F), 1.4F, 12.0F);
        }
    }
    else
    {
        context.MutedText("Preview target is not ready yet.");
    }

    context.MutedText("Drag to orbit, wheel to zoom.");

    static const std::vector<std::string_view> shapes = Labels(shading::Shapes());
    static const std::vector<std::string_view> lights = Labels(shading::LightingPresets());
    static const std::vector<std::string_view> backgrounds = Labels(shading::Backgrounds());

    i32 shape = static_cast<i32>(preview.shape);
    i32 lighting = static_cast<i32>(preview.lighting);
    i32 background = static_cast<i32>(preview.background);

    if (context.Combo("Shape##shading-shape", shapes, shape))
        preview.shape = static_cast<shading::PreviewShape>(shape);

    if (preview.shape == shading::PreviewShape::Mesh)
    {
        const auto& mesh = workspace_->PreviewMeshStatus();

        if (mesh.path.empty())
        {
            context.MutedText(
                "Select a .obj mesh in the tree to preview it. A sphere is "
                "drawn until then.");
        }
        else if (mesh.loaded)
        {
            context.MutedText(std::format(
                "{}: {} vertices, {} triangles{}{}",
                mesh.path.filename().string(),
                mesh.vertices,
                mesh.triangles,
                mesh.hadNormals ? "" : ", generated normals",
                mesh.hadUvs ? "" : ", generated UVs"));
        }

        if (!mesh.error.empty())
        {
            context.ErrorText("Mesh error: " + mesh.error);
        }
    }
    if (context.Combo("Lighting##shading-lighting", lights, lighting))
        preview.lighting = static_cast<shading::LightingPreset>(lighting);
    if (context.Combo("Background##shading-background", backgrounds, background))
        preview.background = static_cast<shading::PreviewBackground>(background);

    f64 sunAzimuth = preview.sunAzimuthDegrees;
    f64 sunElevation = preview.sunElevationDegrees;
    f64 exposure = preview.exposure;
    f64 spin = preview.modelYawDegrees;

    if (context.SliderDouble("Sun azimuth##shading-az", sunAzimuth, -180.0, 180.0))
        preview.sunAzimuthDegrees = static_cast<f32>(sunAzimuth);
    if (context.SliderDouble("Sun elevation##shading-el", sunElevation, -10.0, 89.0))
        preview.sunElevationDegrees = static_cast<f32>(sunElevation);
    if (context.SliderDouble("Exposure##shading-exposure", exposure, 0.05, 8.0))
        preview.exposure = static_cast<f32>(exposure);
    if (context.SliderDouble("Object spin##shading-spin", spin, 0.0, 360.0))
        preview.modelYawDegrees = static_cast<f32>(spin);

    static_cast<void>(context.Checkbox("Animate (drives time in shaders)##shading-animate", preview.animate));
}

void ShadingUi::DrawParameters(editor_ui::PanelContext& context)
{
    const auto parameters = workspace_->Parameters();

    if (parameters.empty())
    {
        context.MutedText(
            "This shader declares no parameters. Add lines such as "
            "'// @param tint color3 0.8 0.8 0.8' to expose some.");
        return;
    }

    if (workspace_->Status().material.empty())
    {
        context.MutedText(
            "Editing a bare shader: values last for this session. Create a "
            "material to save them.");
    }

    for (const auto& parameter : parameters)
    {
        const auto& declaration = parameter.declaration;
        std::array<f64, 4> value = parameter.value;
        bool changed = false;
        const std::string base = declaration.name;

        if (declaration.components == 1U)
        {
            changed = declaration.hasRange
                ? context.SliderDouble(base + "##param", value[0], declaration.minimum, declaration.maximum)
                : context.InputDouble(base + "##param", value[0]);
        }
        else if (declaration.components == 3U)
        {
            math::Double3 vector{value[0], value[1], value[2]};
            if (context.InputDouble3(base + "##param", vector))
            {
                value = {vector.x, vector.y, vector.z, 0.0};
                changed = true;
            }
        }
        else
        {
            for (u32 component = 0U; component < declaration.components; ++component)
            {
                changed |= context.InputDouble(
                    std::format("{}[{}]##param{}", base, component, base), value[component]);
            }
        }

        if (changed)
        {
            Run({},
                [&]
                {
                    workspace_->SetParameter(
                        declaration.name,
                        std::span<const f64>(value.data(), declaration.components),
                        false);
                });
        }

        if (parameter.overridden)
        {
            context.SameLine();
            if (context.Button("Reset##" + base))
            {
                Run("Reset to the shader default.",
                    [&] { workspace_->ResetParameter(declaration.name); });
            }
        }
    }
}

void ShadingUi::DrawEditor(editor_ui::PanelContext& context, const f32 height)
{
    const auto& status = workspace_->Status();

    if (context.PrimaryButton(status.dirty ? "Save and compile *" : "Save and compile"))
    {
        Run("Saved.", [&] { workspace_->Save(); });
    }
    if (context.Button("Revert"))
    {
        Run("Reverted to the file on disk.", [&] { workspace_->Revert(); });
    }
    context.SameLine();
    bool live = workspace_->LiveCompile();
    if (context.Checkbox("Live compile##shading-live", live))
    {
        workspace_->SetLiveCompile(live);
    }

    if (status.changedOnDisk)
    {
        context.ErrorText(
            "Warning: the file changed on disk while you have unsaved edits. "
            "Revert to load it, or Save to overwrite it.");
    }

    if (status.compiled)
    {
        context.MutedText(std::format(
            "Compiled OK in {:.0f} ms. Preview is running this program.",
            status.compileMilliseconds));
    }
    else if (!status.diagnostics.empty())
    {
        context.ErrorText(
            "Compile failed: the preview keeps the last working shader.\n" +
            status.diagnostics);
    }

    std::string text = workspace_->EditorText();
    const f32 width = std::max(context.ContentAvailable().width, 120.0F);
    if (context.InputTextMultiline(
            "##shading-source", text, {.width = width, .height = height}))
    {
        workspace_->SetEditorText(std::move(text));
    }
}
} // namespace orbit::studio_ui

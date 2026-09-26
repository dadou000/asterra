#include <orbit/shading/ShadingWorkspace.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <stdexcept>
#include <unordered_map>

namespace orbit::shading
{
namespace
{
constexpr std::string_view kShaderSuffix = ".shade.hlsl";
constexpr std::string_view kMaterialSuffix = ".orbitshadermaterial";
constexpr f64 kLiveCompileDebounceSeconds = 0.35;

[[nodiscard]] std::string Lower(std::string value)
{
    std::ranges::transform(
        value, value.begin(),
        [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

[[nodiscard]] bool IsUnder(
    const std::filesystem::path& path,
    const std::filesystem::path& folder)
{
    const auto relative = path.lexically_relative(folder);
    return !relative.empty() && *relative.begin() != std::filesystem::path("..");
}

[[nodiscard]] bool IsPreviewable(const content::AssetKind kind) noexcept
{
    return kind == content::AssetKind::ShadingShader ||
        kind == content::AssetKind::ShaderMaterial;
}

[[nodiscard]] std::string StripSuffix(
    std::string name,
    const std::string_view suffix)
{
    if (Lower(name).ends_with(suffix))
    {
        name.resize(name.size() - suffix.size());
    }
    return name;
}

// TOML floats need a decimal point or exponent; "1" would parse as an integer.
[[nodiscard]] std::string Number(const f64 value)
{
    std::string text = std::format("{}", value);
    if (text.find_first_of(".eEn") == std::string::npos)
    {
        text += ".0";
    }
    return text;
}

[[nodiscard]] std::string MaterialText(
    const std::string_view name,
    const std::string_view shader,
    const ShaderParameterLayout& layout,
    const std::map<std::string, std::vector<f64>>& overrides)
{
    std::string text = "[shader_material]\n";
    text += std::format("name = \"{}\"\n", name);
    text += std::format("shader = \"{}\"\n", shader);

    const auto emit = [&](const std::string& parameterName,
                          const std::vector<f64>& values)
    {
        text += "\n[[shader_material.parameter]]\n";
        text += std::format("name = \"{}\"\n", parameterName);

        if (values.size() == 1U)
        {
            text += std::format("value = {}\n", Number(values[0]));
            return;
        }

        text += "value = [";
        for (std::size_t index = 0U; index < values.size(); ++index)
        {
            text += (index == 0U ? "" : ", ") + Number(values[index]);
        }
        text += "]\n";
    };

    // Declaration order first (it is the packing order), then anything the
    // shader no longer declares so a temporary edit never loses data.
    for (const auto& declaration : layout.parameters)
    {
        if (const auto found = overrides.find(declaration.name);
            found != overrides.end())
        {
            emit(found->first, found->second);
        }
    }

    for (const auto& [parameterName, values] : overrides)
    {
        if (layout.Find(parameterName) == nullptr)
        {
            emit(parameterName, values);
        }
    }

    return text;
}
} // namespace

ShadingWorkspace::ShadingWorkspace(
    content::ContentService& content,
    const shader::Compiler* const compiler)
    : content_(content),
      compiler_(compiler),
      seenContentRevision_(content.Revision())
{
}

std::filesystem::path ShadingWorkspace::Normalise(
    const std::filesystem::path& path) const
{
    if (path.empty())
    {
        throw std::invalid_argument("Path must not be empty.");
    }

    std::filesystem::path result = path.lexically_normal();

    // Accept content-relative paths ("Shading/Lunar.shade.hlsl") as well as
    // project-relative ones ("Content/Shading/...").
    if (result.is_relative() &&
        Lower(result.begin()->string()) != "content")
    {
        result = std::filesystem::path("Content") / result;
    }

    return result;
}

// ---------------------------------------------------------------------------
// Tree and organisation
// ---------------------------------------------------------------------------

TreeNode ShadingWorkspace::Tree() const
{
    std::unordered_map<std::string, std::vector<TreeNode>> byParent;

    const auto parentKey = [](const std::filesystem::path& path)
    {
        return Lower(path.parent_path().generic_string());
    };

    for (const auto& folder : content_.Folders())
    {
        TreeNode node;
        node.name = folder.filename().string();
        node.path = folder;
        node.folder = true;
        byParent[parentKey(folder)].push_back(std::move(node));
    }

    for (const auto& asset : content_.All())
    {
        TreeNode node;
        node.name = asset.sourcePath.filename().string();
        node.path = asset.sourcePath;
        node.kind = asset.kind;
        node.previewable = IsPreviewable(asset.kind);
        byParent[parentKey(asset.sourcePath)].push_back(std::move(node));
    }

    const auto build = [&](auto&& self, TreeNode node) -> TreeNode
    {
        if (!node.folder)
        {
            return node;
        }

        auto found = byParent.find(Lower(node.path.generic_string()));
        if (found != byParent.end())
        {
            auto children = std::move(found->second);
            std::ranges::sort(
                children,
                [](const TreeNode& a, const TreeNode& b)
                {
                    if (a.folder != b.folder)
                    {
                        return a.folder;
                    }
                    return Lower(a.name) < Lower(b.name);
                });

            for (auto& child : children)
            {
                node.children.push_back(self(self, std::move(child)));
            }
        }
        return node;
    };

    TreeNode root;
    root.name = "Content";
    root.path = "Content";
    root.folder = true;
    return build(build, std::move(root));
}

void ShadingWorkspace::CreateFolder(const std::filesystem::path& folder)
{
    content_.CreateFolder(Normalise(folder));
    RefreshFromContent();
}

std::filesystem::path ShadingWorkspace::Rename(
    const std::filesystem::path& entry,
    const std::string_view newName)
{
    const auto from = Normalise(entry);
    const auto to = content_.RenameEntry(from, newName);
    RebaseSelection(from, to);
    RebaseMesh(from, to);
    RefreshFromContent();
    return to;
}

std::filesystem::path ShadingWorkspace::Move(
    const std::filesystem::path& entry,
    const std::filesystem::path& folder)
{
    const auto from = Normalise(entry);
    const auto to = content_.MoveEntry(from, Normalise(folder));
    RebaseSelection(from, to);
    RebaseMesh(from, to);
    RefreshFromContent();
    return to;
}

std::filesystem::path ShadingWorkspace::Trash(
    const std::filesystem::path& entry)
{
    const auto from = Normalise(entry);
    const auto to = content_.TrashEntry(from);
    RebaseSelection(from, std::nullopt);
    RebaseMesh(from, std::nullopt);
    RefreshFromContent();
    return to;
}

void ShadingWorkspace::RebaseSelection(
    const std::filesystem::path& from,
    const std::optional<std::filesystem::path>& to)
{
    if (selected_.empty())
    {
        return;
    }

    const bool isSame =
        Lower(selected_.generic_string()) == Lower(from.generic_string());

    if (!isSame && !IsUnder(selected_, from))
    {
        return;
    }

    if (!to.has_value())
    {
        ClearSelection();
        return;
    }

    const auto rebased = isSame
        ? *to
        : *to / selected_.lexically_relative(from);
    Select(rebased);
}

std::filesystem::path ShadingWorkspace::CreateShader(
    const std::filesystem::path& folder,
    const std::string_view name,
    const std::string_view templateName)
{
    const auto source = ShaderTemplateSource(
        templateName.empty() ? std::string_view("lit") : templateName);

    if (!source.has_value())
    {
        std::string names;
        for (const auto candidate : ShaderTemplateNames())
        {
            names += (names.empty() ? "" : ", ") + std::string(candidate);
        }
        throw std::invalid_argument(
            "Unknown shader template '" + std::string(templateName) +
            "'. Available: " + names + ".");
    }

    const std::string baseName = StripSuffix(std::string(name), kShaderSuffix);
    const auto file =
        Normalise(folder) / (baseName + std::string(kShaderSuffix));

    if (content_.FindByPath(file) != nullptr)
    {
        throw std::invalid_argument(
            "A shader already exists at " + file.generic_string());
    }

    content_.WriteText(file, *source);
    RefreshFromContent();
    return file;
}

std::filesystem::path ShadingWorkspace::CreateShaderMaterial(
    const std::filesystem::path& folder,
    const std::string_view name,
    const std::filesystem::path& shader)
{
    const auto shaderPath = Normalise(shader);
    const auto* shaderRecord = content_.FindByPath(shaderPath);

    if (shaderRecord == nullptr ||
        shaderRecord->kind != content::AssetKind::ShadingShader)
    {
        throw std::invalid_argument(
            "A shader material needs an existing *.shade.hlsl shader: " +
            shaderPath.generic_string());
    }

    const std::string baseName =
        StripSuffix(std::string(name), kMaterialSuffix);
    const auto materialFolder = Normalise(folder);
    const auto file =
        materialFolder / (baseName + std::string(kMaterialSuffix));

    if (content_.FindByPath(file) != nullptr)
    {
        throw std::invalid_argument(
            "A shader material already exists at " + file.generic_string());
    }

    const auto relativeShader =
        shaderPath.lexically_relative(materialFolder).generic_string();

    content_.WriteText(
        file,
        MaterialText(baseName, relativeShader, {}, {}));
    RefreshFromContent();
    return file;
}

// ---------------------------------------------------------------------------
// Selection and editing
// ---------------------------------------------------------------------------

std::filesystem::path ShadingWorkspace::ResolveShaderFor(
    const content::AssetRecord& record) const
{
    if (record.kind == content::AssetKind::ShadingShader)
    {
        return record.sourcePath;
    }

    if (record.kind == content::AssetKind::ShaderMaterial &&
        record.shaderMaterial.has_value())
    {
        return (record.sourcePath.parent_path() /
                record.shaderMaterial->shader)
            .lexically_normal();
    }

    return {};
}

void ShadingWorkspace::Select(const std::filesystem::path& asset)
{
    const auto path = Normalise(asset);
    const auto* record = content_.FindByPath(path);

    if (record == nullptr)
    {
        // Folders are selectable (they are the target of "New ...").
        const auto folders = content_.Folders();
        const bool isFolder = std::ranges::any_of(
            folders,
            [&](const auto& folder)
            {
                return Lower(folder.generic_string()) ==
                    Lower(path.generic_string());
            });

        if (!isFolder && Lower(path.generic_string()) != "content")
        {
            throw std::invalid_argument(
                "No such Content entry: " + path.generic_string());
        }
    }

    // A mesh is a preview subject, not something to edit: selecting one
    // switches the preview to it and leaves the open shader alone.
    if (record != nullptr && record->kind == content::AssetKind::Mesh)
    {
        SetPreviewMesh(path);
        return;
    }

    selected_ = path;
    LoadSelection();
}

void ShadingWorkspace::ClearSelection()
{
    selected_.clear();
    LoadSelection();
}

const std::filesystem::path& ShadingWorkspace::Selected() const noexcept
{
    return selected_;
}

void ShadingWorkspace::LoadSelection()
{
    const auto previousShader = status_.shader;

    status_.shader.clear();
    status_.material.clear();
    status_.dirty = false;
    status_.changedOnDisk = false;
    overrides_.clear();
    editPending_ = false;

    const content::AssetRecord* record =
        selected_.empty() ? nullptr : content_.FindByPath(selected_);

    if (record == nullptr || !IsPreviewable(record->kind))
    {
        buffer_.clear();
        diskHash_ = 0U;
        good_.reset();
        layout_ = {};
        status_.compiled = false;
        status_.diagnostics.clear();
        ++status_.programRevision;
        return;
    }

    status_.shader = ResolveShaderFor(*record);

    if (record->kind == content::AssetKind::ShaderMaterial)
    {
        status_.material = record->sourcePath;

        if (record->shaderMaterial.has_value())
        {
            for (const auto& parameter : record->shaderMaterial->parameters)
            {
                overrides_[parameter.name] = parameter.values;
            }
        }
    }

    // A different shader invalidates the previous program; the same shader
    // (for example selecting its material) keeps showing until it recompiles.
    if (status_.shader != previousShader)
    {
        good_.reset();
        layout_ = {};
    }

    try
    {
        buffer_ = content_.ReadText(status_.shader);
    }
    catch (const std::exception& exception)
    {
        buffer_.clear();
        status_.compiled = false;
        status_.diagnostics =
            "Could not read the shader: " + std::string(exception.what());
        good_.reset();
        ++status_.programRevision;
        return;
    }

    diskHash_ = HashSource(buffer_);
    CompileBuffer();
}

const std::string& ShadingWorkspace::EditorText() const noexcept
{
    return buffer_;
}

void ShadingWorkspace::SetEditorText(std::string text)
{
    if (status_.shader.empty())
    {
        return;
    }

    buffer_ = std::move(text);
    status_.dirty = HashSource(buffer_) != diskHash_;
    editPending_ = true;
    sinceEditSeconds_ = 0.0;
}

void ShadingWorkspace::Save()
{
    if (status_.shader.empty())
    {
        throw std::logic_error("No shader is selected.");
    }

    content_.WriteText(status_.shader, buffer_);
    diskHash_ = HashSource(buffer_);
    status_.dirty = false;
    status_.changedOnDisk = false;
    seenContentRevision_ = content_.Revision();
    CompileBuffer();
}

void ShadingWorkspace::Revert()
{
    if (status_.shader.empty())
    {
        return;
    }

    buffer_ = content_.ReadText(status_.shader);
    diskHash_ = HashSource(buffer_);
    status_.dirty = false;
    status_.changedOnDisk = false;
    CompileBuffer();
}

void ShadingWorkspace::WriteSource(
    const std::filesystem::path& shader,
    const std::string_view text)
{
    const auto path = Normalise(shader);

    if (!Lower(path.generic_string()).ends_with(kShaderSuffix))
    {
        throw std::invalid_argument(
            "Shader sources must be named *.shade.hlsl: " +
            path.generic_string());
    }

    // Writing a source opens it, like saving in an editor: the tab follows so
    // the change is visible and its diagnostics are the ones reported.
    if (content_.FindByPath(path) == nullptr)
    {
        content_.WriteText(path, text);
    }

    Select(path);
    SetEditorText(std::string(text));
    Save();
}

std::string ShadingWorkspace::ReadSource(
    const std::filesystem::path& shader) const
{
    return content_.ReadText(Normalise(shader));
}

void ShadingWorkspace::SetLiveCompile(const bool enabled) noexcept
{
    liveCompile_ = enabled;
}

bool ShadingWorkspace::LiveCompile() const noexcept
{
    return liveCompile_;
}

void ShadingWorkspace::Recompile()
{
    if (!status_.shader.empty())
    {
        CompileBuffer();
    }
}

void ShadingWorkspace::CompileBuffer()
{
    lastCompiledHash_ = HashSource(buffer_);

    if (compiler_ == nullptr)
    {
        status_.compiled = false;
        status_.diagnostics = "No shader compiler is available.";
        return;
    }

    const std::string name = status_.shader.filename().string();
    ShadingProgram program =
        CompileShadingProgram(*compiler_, name, buffer_);

    status_.compileMilliseconds = program.compileMilliseconds;

    if (program.ok)
    {
        layout_ = program.layout;
        good_ = std::make_unique<ShadingProgram>(std::move(program));
        status_.compiled = true;
        status_.diagnostics.clear();
        ++status_.programRevision;
        return;
    }

    // Failed: keep the last good program (and its layout) live so the preview
    // keeps drawing while the author fixes the error.
    status_.compiled = false;
    status_.diagnostics = std::move(program.diagnostics);
}

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

std::vector<ParameterValue> ShadingWorkspace::Parameters() const
{
    std::vector<ParameterValue> result;

    for (const auto& declaration : layout_.parameters)
    {
        ParameterValue value;
        value.declaration = declaration;
        value.value = declaration.defaults;

        if (const auto found = overrides_.find(declaration.name);
            found != overrides_.end() &&
            found->second.size() == declaration.components)
        {
            for (std::size_t index = 0U; index < found->second.size(); ++index)
            {
                value.value[index] = found->second[index];
            }
            value.overridden = true;
        }

        result.push_back(std::move(value));
    }

    return result;
}

void ShadingWorkspace::SetParameter(
    const std::string_view name,
    const std::span<const f64> values,
    const bool persistNow)
{
    const auto* declaration = layout_.Find(name);

    if (declaration == nullptr)
    {
        throw std::invalid_argument(
            "The shader declares no parameter named '" + std::string(name) +
            "'.");
    }

    if (values.size() != declaration->components)
    {
        throw std::invalid_argument(
            "Parameter '" + std::string(name) + "' takes " +
            std::to_string(declaration->components) + " value(s).");
    }

    for (const f64 value : values)
    {
        if (!std::isfinite(value))
        {
            throw std::invalid_argument(
                "Parameter values must be finite numbers.");
        }
    }

    overrides_[std::string(name)] =
        std::vector<f64>(values.begin(), values.end());

    if (persistNow)
    {
        PersistMaterial();
        return;
    }

    parameterWritePending_ = true;
    sinceParameterEditSeconds_ = 0.0;
}

void ShadingWorkspace::ResetParameter(const std::string_view name)
{
    overrides_.erase(std::string(name));
    PersistMaterial();
}

void ShadingWorkspace::FlushParameterWrites()
{
    if (parameterWritePending_)
    {
        PersistMaterial();
    }
}

void ShadingWorkspace::PersistMaterial()
{
    parameterWritePending_ = false;

    if (status_.material.empty())
    {
        return;
    }

    const auto* record = content_.FindByPath(status_.material);
    if (record == nullptr || !record->shaderMaterial.has_value())
    {
        return;
    }

    content_.WriteText(
        status_.material,
        MaterialText(
            record->name,
            record->shaderMaterial->shader.generic_string(),
            layout_,
            overrides_));
    seenContentRevision_ = content_.Revision();
}

std::array<f32, kMaxParameterFloats> ShadingWorkspace::PackedParameters() const
{
    std::array<f32, kMaxParameterFloats> packed{};

    for (const auto& parameter : Parameters())
    {
        for (u32 component = 0U; component < parameter.declaration.components;
             ++component)
        {
            const u32 slot = parameter.declaration.offset + component;
            if (slot < packed.size())
            {
                packed[slot] = static_cast<f32>(parameter.value[component]);
            }
        }
    }

    return packed;
}

// ---------------------------------------------------------------------------
// Preview and per-frame
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Preview mesh
// ---------------------------------------------------------------------------

void ShadingWorkspace::SetPreviewMesh(const std::filesystem::path& mesh)
{
    const auto path = Normalise(mesh);
    const auto* record = content_.FindByPath(path);

    if (record == nullptr || record->kind != content::AssetKind::Mesh)
    {
        throw std::invalid_argument(
            "Not a mesh asset in Content: " + path.generic_string());
    }

    if (Lower(path.extension().string()) != ".obj")
    {
        throw std::invalid_argument(
            "Only Wavefront .obj meshes can be previewed (got '" +
            path.extension().string() + "').");
    }

    const bool changedTarget = meshStatus_.path != path;
    meshStatus_.path = path;
    preview_.mesh = path.generic_string();
    preview_.shape = PreviewShape::Mesh;
    LoadPreviewMesh(changedTarget);
}

void ShadingWorkspace::ClearPreviewMesh()
{
    mesh_.reset();
    meshDiskHash_ = 0U;
    const u64 revision = meshStatus_.revision + 1U;
    meshStatus_ = {};
    meshStatus_.revision = revision;
    preview_.mesh.clear();
}

void ShadingWorkspace::LoadPreviewMesh(const bool changedTarget)
{
    if (changedTarget)
    {
        // A different mesh must not show the previous one while it loads or if
        // it fails; the same file re-saved with an error keeps the last good.
        mesh_.reset();
    }

    try
    {
        const std::string text = content_.ReadText(meshStatus_.path);
        meshDiskHash_ = HashSource(text);

        auto parsed = std::make_shared<MeshData>(ParseObj(text));
        meshStatus_.vertices = static_cast<u32>(parsed->vertices.size());
        meshStatus_.triangles = parsed->TriangleCount();
        meshStatus_.sourceRadius = parsed->sourceRadius;
        meshStatus_.hadNormals = parsed->hadNormals;
        meshStatus_.hadUvs = parsed->hadUvs;
        meshStatus_.error.clear();
        mesh_ = std::move(parsed);
    }
    catch (const std::exception& exception)
    {
        meshStatus_.error =
            meshStatus_.path.filename().string() + ": " + exception.what();
    }

    meshStatus_.loaded = mesh_ != nullptr;
    ++meshStatus_.revision;
}

// A mesh saved elsewhere (an external modelling tool) reloads with no action,
// like a shader; a deleted file keeps the last good mesh and reports it.
void ShadingWorkspace::RefreshMesh()
{
    if (meshStatus_.path.empty())
    {
        return;
    }

    if (content_.FindByPath(meshStatus_.path) == nullptr)
    {
        if (meshStatus_.error.empty())
        {
            meshStatus_.error =
                meshStatus_.path.filename().string() +
                ": the file is gone; showing the last loaded mesh.";
            ++meshStatus_.revision;
        }
        return;
    }

    try
    {
        if (HashSource(content_.ReadText(meshStatus_.path)) == meshDiskHash_ &&
            meshStatus_.error.empty())
        {
            return;
        }
    }
    catch (const std::exception&)
    {
        return;
    }

    LoadPreviewMesh(false);
}

void ShadingWorkspace::RebaseMesh(
    const std::filesystem::path& from,
    const std::optional<std::filesystem::path>& to)
{
    if (meshStatus_.path.empty())
    {
        return;
    }

    const bool isSame =
        Lower(meshStatus_.path.generic_string()) == Lower(from.generic_string());

    if (!isSame && !IsUnder(meshStatus_.path, from))
    {
        return;
    }

    if (!to.has_value())
    {
        ClearPreviewMesh();
        if (preview_.shape == PreviewShape::Mesh)
        {
            preview_.shape = PreviewShape::Sphere;
        }
        return;
    }

    meshStatus_.path =
        isSame ? *to : *to / meshStatus_.path.lexically_relative(from);
    preview_.mesh = meshStatus_.path.generic_string();
}

const MeshStatus& ShadingWorkspace::PreviewMeshStatus() const noexcept
{
    return meshStatus_;
}

std::shared_ptr<const MeshData> ShadingWorkspace::PreviewMeshData() const noexcept
{
    return mesh_;
}

PreviewState& ShadingWorkspace::Preview() noexcept { return preview_; }
const PreviewState& ShadingWorkspace::Preview() const noexcept
{
    return preview_;
}
f32 ShadingWorkspace::PreviewTime() const noexcept
{
    return static_cast<f32>(std::fmod(time_, 3600.0));
}

void ShadingWorkspace::Update(const f64 deltaSeconds)
{
    if (preview_.animate)
    {
        time_ += deltaSeconds;
    }

    if (content_.Revision() != seenContentRevision_)
    {
        RefreshFromContent();
    }

    if (parameterWritePending_)
    {
        sinceParameterEditSeconds_ += deltaSeconds;

        if (sinceParameterEditSeconds_ >= kLiveCompileDebounceSeconds)
        {
            PersistMaterial();
        }
    }

    if (liveCompile_ && editPending_)
    {
        sinceEditSeconds_ += deltaSeconds;

        if (sinceEditSeconds_ >= kLiveCompileDebounceSeconds)
        {
            editPending_ = false;

            if (HashSource(buffer_) != lastCompiledHash_)
            {
                CompileBuffer();
            }
        }
    }
}

// Reacts to content changes from any source: this panel, another tool, or an
// external editor saving a file (delivered by the central hot-iteration
// watcher).
void ShadingWorkspace::RefreshFromContent()
{
    seenContentRevision_ = content_.Revision();
    RefreshMesh();

    if (selected_.empty())
    {
        return;
    }

    const auto* record = content_.FindByPath(selected_);

    if (record == nullptr)
    {
        // Selected asset is gone (trashed or renamed outside the tab). A
        // selected folder that still exists stays selected.
        const auto folders = content_.Folders();
        const bool stillFolder = std::ranges::any_of(
            folders,
            [&](const auto& folder)
            {
                return Lower(folder.generic_string()) ==
                    Lower(selected_.generic_string());
            });

        if (!stillFolder)
        {
            ClearSelection();
        }
        return;
    }

    if (!IsPreviewable(record->kind))
    {
        return;
    }

    const auto shaderPath = ResolveShaderFor(*record);

    // A material was retargeted at another shader.
    if (shaderPath != status_.shader)
    {
        LoadSelection();
        return;
    }

    if (record->kind == content::AssetKind::ShaderMaterial &&
        record->shaderMaterial.has_value() &&
        !parameterWritePending_)
    {
        std::map<std::string, std::vector<f64>> fromDisk;
        for (const auto& parameter : record->shaderMaterial->parameters)
        {
            fromDisk[parameter.name] = parameter.values;
        }
        overrides_ = std::move(fromDisk);
    }

    std::string onDisk;
    try
    {
        onDisk = content_.ReadText(status_.shader);
    }
    catch (const std::exception&)
    {
        return;
    }

    const u64 hash = HashSource(onDisk);
    if (hash == diskHash_)
    {
        return;
    }

    if (status_.dirty)
    {
        // Never clobber unsaved edits with an external change.
        status_.changedOnDisk = true;
        return;
    }

    buffer_ = std::move(onDisk);
    diskHash_ = hash;
    CompileBuffer();
}

const ShaderStatus& ShadingWorkspace::Status() const noexcept
{
    return status_;
}

const ShadingProgram* ShadingWorkspace::Program() const noexcept
{
    return good_.get();
}

void ShadingWorkspace::ReportPipelineFailure(
    const u64 programRevision,
    std::string message)
{
    if (programRevision == status_.programRevision)
    {
        status_.compiled = false;
        status_.diagnostics = "Pipeline creation failed: " + std::move(message);
    }
}

content::ContentService& ShadingWorkspace::Content() noexcept
{
    return content_;
}

const content::ContentService& ShadingWorkspace::Content() const noexcept
{
    return content_;
}
} // namespace orbit::shading

#pragma once

#include <orbit/content/ContentService.hpp>
#include <orbit/core/Types.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/shading/ShaderProgram.hpp>
#include <orbit/shading/ShadingContract.hpp>

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::shading
{
struct TreeNode
{
    std::string name;
    // Project-relative, e.g. "Content/Shading/Lunar.shade.hlsl".
    std::filesystem::path path;
    bool folder{false};
    std::optional<content::AssetKind> kind;
    // True for kinds the Shading tab can preview (shaders, shader materials).
    bool previewable{false};
    std::vector<TreeNode> children;
};

struct ShaderStatus
{
    // The shader being edited/previewed and, when a shader material was
    // selected, that material. Both project-relative; empty when none.
    std::filesystem::path shader;
    std::filesystem::path material;
    // Last compile attempt succeeded (the preview shows this program).
    bool compiled{false};
    std::string diagnostics;
    // Bumps whenever the program the preview should draw with changes.
    u64 programRevision{0U};
    f64 compileMilliseconds{0.0};
    // Editor buffer differs from the file on disk.
    bool dirty{false};
    // The file changed on disk while the buffer had unsaved edits.
    bool changedOnDisk{false};
};

struct ParameterValue
{
    ShaderParameterDecl declaration;
    std::array<f64, 4> value{};
    // Differs from the shader's declared default.
    bool overridden{false};
};

// The Shading tab's model. The UI panel and the RPC/MCP methods are both thin
// clients of this class, so anything a person can do in the tab is reachable by
// an agent (ORBIT_UI_RULES section 22) and there is one code path for each.
//
// It owns no GPU state: the renderer polls Program()/ProgramRevision().
//
// Hot path: a shader saved by any editor reaches ContentService through the
// central hot-iteration watcher; Update() notices the content revision change,
// reloads and recompiles, and the renderer swaps its pipeline at a frame
// boundary. The running Studio is never restarted for a shader edit.
class ShadingWorkspace
{
public:
    // `compiler` may be null (compiles then report a diagnostic); the
    // workspace never owns it.
    ShadingWorkspace(
        content::ContentService& content,
        const shader::Compiler* compiler);

    // ---- Tree and organisation ------------------------------------------
    [[nodiscard]] TreeNode Tree() const;
    void CreateFolder(const std::filesystem::path& folder);
    std::filesystem::path Rename(
        const std::filesystem::path& entry,
        std::string_view newName);
    std::filesystem::path Move(
        const std::filesystem::path& entry,
        const std::filesystem::path& folder);
    std::filesystem::path Trash(const std::filesystem::path& entry);

    // Creates `<folder>/<name>.shade.hlsl` from a template (see
    // ShaderTemplateNames) and returns its path. Refuses to overwrite.
    std::filesystem::path CreateShader(
        const std::filesystem::path& folder,
        std::string_view name,
        std::string_view templateName);

    // Creates `<folder>/<name>.orbitshadermaterial` bound to a shader, with no
    // overrides (the shader's declared defaults apply).
    std::filesystem::path CreateShaderMaterial(
        const std::filesystem::path& folder,
        std::string_view name,
        const std::filesystem::path& shader);

    // ---- Selection and editing ------------------------------------------
    // Selecting a shader or a shader material loads the shader into the editor
    // and compiles it. Other asset kinds can be selected but are not
    // previewable yet (the selection is kept; the preview is cleared).
    void Select(const std::filesystem::path& asset);
    void ClearSelection();
    [[nodiscard]] const std::filesystem::path& Selected() const noexcept;

    [[nodiscard]] const std::string& EditorText() const noexcept;
    void SetEditorText(std::string text);
    // Writes the buffer to the shader file (atomic) and compiles it.
    void Save();
    // Reloads the buffer from disk, discarding edits.
    void Revert();
    // Writes new source directly (RPC/MCP path): saves and compiles.
    void WriteSource(
        const std::filesystem::path& shader,
        std::string_view text);
    [[nodiscard]] std::string ReadSource(
        const std::filesystem::path& shader) const;

    // Compile the buffer as it is typed (debounced) without saving.
    void SetLiveCompile(bool enabled) noexcept;
    [[nodiscard]] bool LiveCompile() const noexcept;

    // Recompiles the current buffer now, regardless of edits.
    void Recompile();

    // ---- Parameters ------------------------------------------------------
    [[nodiscard]] std::vector<ParameterValue> Parameters() const;
    // Sets a parameter by name. With a shader material selected the override
    // is written back to the material asset; with a bare shader it lasts for
    // the session. `values.size()` must equal the parameter's components.
    //
    // `persistNow` false defers the write to Update() after a short pause, so
    // dragging a slider does not rewrite the asset every frame; RPC callers use
    // the default (immediate) so the file is saved when the call returns.
    void SetParameter(
        std::string_view name,
        std::span<const f64> values,
        bool persistNow = true);
    void ResetParameter(std::string_view name);
    // Writes any deferred parameter edit now.
    void FlushParameterWrites();
    // The packed block the preview pipeline consumes.
    [[nodiscard]] std::array<f32, kMaxParameterFloats> PackedParameters() const;

    // ---- Preview ---------------------------------------------------------
    [[nodiscard]] PreviewState& Preview() noexcept;
    [[nodiscard]] const PreviewState& Preview() const noexcept;
    [[nodiscard]] f32 PreviewTime() const noexcept;

    // ---- Per frame -------------------------------------------------------
    // Advances the preview clock, notices external file changes, and runs the
    // debounced live compile. Cheap when nothing changed.
    void Update(f64 deltaSeconds);

    [[nodiscard]] const ShaderStatus& Status() const noexcept;
    // Last program that compiled (may lag the buffer while it has errors).
    [[nodiscard]] const ShadingProgram* Program() const noexcept;
    // The renderer could not build a pipeline from `programRevision`.
    void ReportPipelineFailure(u64 programRevision, std::string message);

    [[nodiscard]] content::ContentService& Content() noexcept;
    [[nodiscard]] const content::ContentService& Content() const noexcept;

private:
    [[nodiscard]] std::filesystem::path Normalise(
        const std::filesystem::path& path) const;
    [[nodiscard]] std::filesystem::path ResolveShaderFor(
        const content::AssetRecord& record) const;
    void LoadSelection();
    void CompileBuffer();
    void RebuildParameterValues();
    void PersistMaterial();
    void RefreshFromContent();
    void RebaseSelection(
        const std::filesystem::path& from,
        const std::optional<std::filesystem::path>& to);

    content::ContentService& content_;
    const shader::Compiler* compiler_{nullptr};

    std::filesystem::path selected_;
    ShaderStatus status_;
    std::string buffer_;
    u64 diskHash_{0U};
    u64 lastCompiledHash_{0U};
    f64 sinceEditSeconds_{0.0};
    bool liveCompile_{true};
    bool editPending_{false};
    u64 seenContentRevision_{0U};
    bool parameterWritePending_{false};
    f64 sinceParameterEditSeconds_{0.0};

    std::unique_ptr<ShadingProgram> good_;
    ShaderParameterLayout layout_;
    // name -> components; overrides the shader's declared defaults. Loaded
    // from the selected material and edited through SetParameter.
    std::map<std::string, std::vector<f64>> overrides_;

    PreviewState preview_;
    f64 time_{0.0};
};
} // namespace orbit::shading

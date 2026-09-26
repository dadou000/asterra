#pragma once

#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/shading/ShadingWorkspace.hpp>

#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace orbit::studio_ui
{
// The Shading tab: a Content tree with folders, a live-compiled HLSL editor,
// parameter controls and a preview of the shader on a selectable shape under
// selectable lighting.
//
// The panel owns presentation state only. Every operation is a call into
// shading::ShadingWorkspace, which the shading.* RPC/MCP methods drive too, so
// anything done here can be done by an agent and vice versa
// (docs/ORBIT_SHADING.md, ORBIT_UI_RULES.md sections 13 and 22).
class ShadingUi
{
public:
    // `previewColor` returns the preview render target (null until it exists).
    ShadingUi(
        shading::ShadingWorkspace& workspace,
        std::function<rhi::Texture*()> previewColor);

    void Register(editor_ui::EditorUi& ui);

    // The preview size the panel wants this frame, if it changed; the owner
    // resizes its render view to it before the next frame.
    [[nodiscard]] std::optional<std::pair<u32, u32>> TakePreviewResizeRequest();

    inline static constexpr editor_ui::PanelId kPanelId{
        .high = 0x4f52424954535455ULL,
        .low = 0x53484144494e4700ULL
    };

private:
    void Draw(editor_ui::PanelContext& context);
    void DrawTree(editor_ui::PanelContext& context);
    void DrawTreeNode(
        editor_ui::PanelContext& context,
        const shading::TreeNode& node);
    void DrawWorkArea(editor_ui::PanelContext& context);
    void DrawPreview(editor_ui::PanelContext& context);
    void DrawParameters(editor_ui::PanelContext& context);
    void DrawEditor(editor_ui::PanelContext& context, f32 height);

    // Runs a workspace action, reporting its result (or failure) next to the
    // controls that triggered it instead of in another panel.
    void Run(const std::string& success, const std::function<void()>& action);

    [[nodiscard]] std::string TargetFolder() const;

    shading::ShadingWorkspace* workspace_{nullptr};
    std::function<rhi::Texture*()> previewColor_;
    std::optional<std::pair<u32, u32>> pendingResize_;
    std::pair<u32, u32> lastRequestedSize_{0U, 0U};

    std::string nameBuffer_{"NewShader"};
    i32 templateIndex_{0};
    std::string status_;
    bool statusIsError_{false};
};
} // namespace orbit::studio_ui

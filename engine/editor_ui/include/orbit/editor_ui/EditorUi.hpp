#pragma once

#include <orbit/core/StrongId.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/platform/Window.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/rhi/Queue.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::editor_ui
{
struct PanelIdTag;
using PanelId = core::StrongId<PanelIdTag>;

struct UiSize
{
    f32 width{0.0F};
    f32 height{0.0F};
};

struct TreeItemInteraction
{
    bool open{false};
    bool clicked{false};
    bool rightClicked{false};
};

struct ImageInteraction
{
    bool hovered{false};
    bool clicked{false};
    bool doubleClicked{false};
    bool rightClicked{false};
    f32 u{0.0F};
    f32 v{0.0F};
};

struct ActionPresentation
{
    std::string label;
    bool enabled{true};
    std::string disabledReason;
    std::function<void()> invoke;
};

class PanelContext
{
public:
    void Text(std::string_view text);
    void Separator();
    [[nodiscard]] bool Button(std::string_view label);
    [[nodiscard]] bool InputText(std::string_view label, std::string& value);
    [[nodiscard]] UiSize ContentAvailable() const;
    [[nodiscard]] bool Selectable(std::string_view label, bool selected);
    [[nodiscard]] TreeItemInteraction TreeItem(std::string_view label, bool selected);
    void TreePop();
    [[nodiscard]] ImageInteraction Image(rhi::Texture& texture, UiSize size);
    [[nodiscard]] bool Checkbox(std::string_view label, bool& value);
    [[nodiscard]] bool InputDouble(std::string_view label, f64& value);
    [[nodiscard]] bool InputInteger(std::string_view label, i64& value);
    [[nodiscard]] bool InputDouble3(std::string_view label, math::Double3& value);
    [[nodiscard]] bool ControlDown() const noexcept;
    [[nodiscard]] bool BeginDragSource();
    void SetDragPayload(std::string_view type, std::span<const std::byte> bytes);
    void EndDragSource();
    [[nodiscard]] std::optional<std::vector<std::byte>> AcceptDragPayload(std::string_view type);
    void Toolbar(std::span<const ActionPresentation> actions);
    void ContextMenu(std::string_view id, std::span<const ActionPresentation> actions, bool openRequested);
    void RadialMenu(std::string_view id, std::span<const ActionPresentation> actions, bool openRequested);
    void SameLine();

private:
    PanelContext(
        bool forceTreeOpen,
        std::vector<std::string>* automationTrace) noexcept;

    void TraceWidget(
        std::string_view label) const;

    bool forceTreeOpen_{false};
    std::vector<std::string>* automationTrace_{nullptr};

    friend class EditorUi;
};

// Where a panel lives in the first-run (and Reset Layout) dock arrangement.
// Auto is for panels the editor cannot know about, such as plugin panels; it
// resolves to the Right tab group.
enum class DockRegion : u8
{
    Auto,
    Center,
    Left,
    Right,
    Bottom
};

struct PanelDefinition
{
    PanelId id{};
    std::string title;
    bool defaultOpen{true};
    // Dedicated shell panels can opt into the central dock node. This is
    // applied every frame so a stale or corrupt layout cannot strand a
    // required panel as an unusably small floating window.
    bool dockToMainViewport{false};
    DockRegion defaultDock{DockRegion::Auto};
    // Tab order inside the default region: lower comes first, and the first
    // tab is the one shown. Ties keep registration order.
    i32 dockOrder{100};
    // Hard lower bound on the panel's size, in logical pixels. Zero leaves
    // that axis unconstrained.
    UiSize minSize{};
    std::function<void(PanelContext&)> draw;
};

struct DockLayoutFractions
{
    f32 left{0.20F};
    f32 right{0.26F};
    f32 bottom{0.22F};
};

struct DockAssignment
{
    PanelId panel{};
    DockRegion region{DockRegion::Center};
};

// Successive splits for ImGui's DockBuilderSplitNode. Bottom is split from the
// whole dock space first (full width); left and right are then split from what
// remains, so their ratios are relative to that remainder rather than to the
// whole. A zero ratio means the region has no panels and is not split off.
struct DockSplitPlan
{
    f32 bottomOfRoot{0.0F};
    f32 leftOfRemainder{0.0F};
    f32 rightOfRemainder{0.0F};
};

// Assigns each panel that participates in the default layout (everything not
// pinned with dockToMainViewport) to a concrete region, ordered by dockOrder
// (ties in registration order).
[[nodiscard]] std::vector<DockAssignment> AssignDefaultDock(
    std::span<const PanelDefinition> panels);

[[nodiscard]] DockSplitPlan PlanDockSplits(
    std::span<const DockAssignment> assignments,
    const DockLayoutFractions& fractions);

// True when a saved ImGui layout actually docks at least one window. A layout
// where every panel floats (or an empty one) is treated as "no layout" and
// gets the default arrangement.
[[nodiscard]] bool LayoutTextHasDockedPanels(
    std::string_view layoutText) noexcept;

// Deterministic probe of one panel's live window. Used by smoke validation.
struct PanelLayoutProbe
{
    bool found{false};
    bool docked{false};
    f32 x{0.0F};
    f32 y{0.0F};
    f32 width{0.0F};
    f32 height{0.0F};
};

struct MenuAction
{
    std::string menu;
    std::string label;
    std::function<void()> invoke;
    std::function<bool()> enabled;
};

class EditorUi
{
public:
    EditorUi(
        rhi::Device& device,
        rhi::Queue& graphicsQueue,
        const shader::Compiler& compiler,
        std::filesystem::path layoutPath);

    ~EditorUi();

    EditorUi(const EditorUi&) = delete;
    EditorUi& operator=(const EditorUi&) = delete;

    void RegisterPanel(PanelDefinition panel);

    // Replaces an existing panel definition while preserving its current
    // open/closed state, or registers it when the ID is new. This is the
    // dynamic extension path used by hot-reloadable editor plugins.
    void UpsertPanel(PanelDefinition panel);

    [[nodiscard]] bool UnregisterPanel(PanelId id);
    [[nodiscard]] bool HasPanel(PanelId id) const noexcept;
    [[nodiscard]] bool SetPanelOpen(PanelId id, bool open) noexcept;
    [[nodiscard]] bool PanelOpen(PanelId id) const noexcept;

    // Deterministic real-UI validation seam. Normal Studio leaves this off.
    // Smoke mode can expand tree nodes and record controls that actually pass
    // through the live Dear ImGui draw path.
    void SetAutomationUiProbe(
        bool expandTrees,
        bool traceWidgets) noexcept;
    void ClearAutomationUiTrace();
    [[nodiscard]] bool AutomationUiTraceContains(
        std::string_view label) const;
    [[nodiscard]] std::size_t AutomationUiTraceSize() const noexcept;
    [[nodiscard]] PanelLayoutProbe AutomationPanelLayout(PanelId id) const;
    // Work area available to docked panels (below the main menu bar), in the
    // same logical pixels as AutomationPanelLayout.
    [[nodiscard]] UiSize AutomationWorkArea() const;

    // Discards the current arrangement and re-applies the default dock layout
    // on the next DrawStudioShell.
    void ResetLayout() noexcept;

    void RegisterMenuAction(MenuAction action);
    void BeginFrame(platform::Window& window, f64 deltaSeconds);
    void DrawStudioShell();
    void Render(rhi::CommandList& commands, rhi::Texture& target, u32 targetWidth, u32 targetHeight);
    [[nodiscard]] bool WantsMouse() const noexcept;
    [[nodiscard]] bool WantsKeyboard() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::editor_ui

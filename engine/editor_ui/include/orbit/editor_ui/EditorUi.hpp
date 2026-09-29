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
// The UI scale the running EditorUi was built with (see EditorUi's `uiScale`
// constructor parameter): 1.0 unless the window's DpiScale() was above 1.0 at
// startup. ImGui's own font size and style metrics already scale with it;
// this is for a panel's *own* hardcoded pixel constants (a thumbnail size, a
// preview width clamp) that would otherwise stay tiny on a high-DPI display.
// Valid only after an EditorUi has been constructed.
[[nodiscard]] f32 CurrentUiScale() noexcept;

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
    // Only filled by InteractiveImage: a left-drag that started on the image
    // (delta since the previous frame, in pixels) and the wheel over it.
    bool dragging{false};
    f32 dragDeltaX{0.0F};
    f32 dragDeltaY{0.0F};
    f32 wheel{0.0F};
};

struct CanvasInteraction
{
    bool hovered{false};
    bool clicked{false};
    bool doubleClicked{false};
    bool rightClicked{false};
    bool dragging{false};
    bool leftDown{false};
    bool leftReleased{false};
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

class ShellBandRegistry;

class PanelContext
{
public:
    void Text(std::string_view text);
    void MutedText(std::string_view text);
    void Heading(std::string_view text);
    void Separator();
    // Collapsible group. Returns whether the body is open; nothing to pop.
    // The label doubles as the ImGui ID, so keep labels unique per panel.
    [[nodiscard]] bool Section(std::string_view label, bool defaultOpen = true);
    // "Label   value" row with the label muted and the value wrapped.
    void KeyValue(std::string_view label, std::string_view value);
    [[nodiscard]] bool Button(std::string_view label);
    [[nodiscard]] bool PrimaryButton(std::string_view label);
    [[nodiscard]] bool InputText(std::string_view label, std::string& value);
    // Multi-line editor for code. The string grows as the user types; Tab
    // inserts a tab. Returns true on the frame it changed.
    [[nodiscard]] bool InputTextMultiline(
        std::string_view label,
        std::string& value,
        UiSize size);
    // Drop-down selecting one of `items`; `index` is the current choice.
    [[nodiscard]] bool Combo(
        std::string_view label,
        std::span<const std::string_view> items,
        i32& index);
    // Equal-width one-row choice surface for a small mutually exclusive set.
    // `id` is an automation/ImGui identity and is not rendered as a label.
    [[nodiscard]] bool SegmentedControl(
        std::string_view id,
        std::span<const std::string_view> items,
        i32& index);
    [[nodiscard]] bool SliderDouble(
        std::string_view label,
        f64& value,
        f64 minimum,
        f64 maximum);
    // Wrapped text in the error colour (never the only cue: callers also say
    // "error" or "failed" in the text itself).
    void ErrorText(std::string_view text);
    // A scrollable sub-region. Always pair with EndChild, even when it
    // returns false.
    [[nodiscard]] bool BeginChild(
        std::string_view id,
        UiSize size,
        bool border = false);
    void EndChild();
    [[nodiscard]] UiSize ContentAvailable() const;
    [[nodiscard]] bool Selectable(std::string_view label, bool selected);
    [[nodiscard]] TreeItemInteraction TreeItem(std::string_view label, bool selected);
    void TreePop();
    [[nodiscard]] ImageInteraction Image(rhi::Texture& texture, UiSize size);
    // An image that captures drags and the mouse wheel (for orbiting a
    // preview). `id` must be unique in the panel.
    [[nodiscard]] ImageInteraction InteractiveImage(
        std::string_view id,
        rhi::Texture& texture,
        UiSize size);
    [[nodiscard]] CanvasInteraction Canvas(std::string_view id, UiSize size);
    void CanvasLine(math::Float2 a, math::Float2 b, math::Float4 color, f32 thickness = 1.0F);
    void CanvasCircle(math::Float2 center, f32 radiusPixels, math::Float4 color, bool filled = true, f32 thickness = 1.0F);
    void CanvasText(math::Float2 position, math::Float4 color, std::string_view text);
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
    // Generic popup surface for rich transient UI such as command palettes.
    // Call EndPopup only when BeginPopup returns true. A zero size axis is
    // auto-fit by ImGui; non-zero dimensions apply when the popup appears.
    [[nodiscard]] bool BeginPopup(
        std::string_view id,
        bool openRequested,
        UiSize size = {});
    void EndPopup();
    void CloseCurrentPopup();
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
    math::Float2 canvasOrigin_{};
    UiSize canvasSize_{};
    bool canvasActive_{false};

    friend class EditorUi;
    friend class ShellBandRegistry;
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
    // Size used the first time the panel appears when it has no saved size
    // (for example a new panel opened in a project whose saved layout predates
    // it, where it floats). Zero leaves ImGui's default. Docked panels ignore
    // it; the user's own resizing always wins.
    UiSize defaultSize{};
    std::function<void(PanelContext&)> draw;
};

struct DockLayoutFractions
{
    // Keep the main workspace visually dominant on a fresh/reset layout.
    // Side panels remain large enough for hierarchy/properties work, while
    // the activity/output strip no longer consumes nearly a quarter of the
    // editor before the user asks for it.
    f32 left{0.18F};
    f32 right{0.22F};
    f32 bottom{0.15F};
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
    // Display-only hint (for example "Ctrl+Z"); the binding itself lives in
    // the shortcut registry.
    std::string shortcut;
};

enum class ShellBandEdge : u8
{
    Top,
    Bottom
};

// Persistent, non-dockable row attached directly to the Studio shell. Bands
// reserve viewport work-area space on the requested edge so docked panels
// never sit underneath them. Ordering is stable within each edge, and the draw
// callback uses the same presentation surface as ordinary panels.
struct ShellBandDefinition
{
    std::string id;
    i32 order{0};
    f32 height{40.0F};
    ShellBandEdge edge{ShellBandEdge::Top};
    std::function<void(PanelContext&)> draw;
};

// Upsert/remove apply to the currently active EditorUi/ImGui context. Studio
// uses stable IDs so hot reload or duplicate registration replaces the band
// definition instead of creating extra rows.
void UpsertShellBand(ShellBandDefinition band);
[[nodiscard]] bool RemoveShellBand(std::string_view id) noexcept;

// One row of the View menu: a panel and whether it is currently open. Rows are
// ordered by dock region and then tab order so the menu mirrors the layout.
struct PanelMenuEntry
{
    PanelId panel{};
    std::string title;
    DockRegion region{DockRegion::Center};
    bool open{false};
};

// panelOpen is parallel to panels. Panels pinned to the main viewport (shell
// panels such as the project browser) are not user-toggleable and are skipped.
[[nodiscard]] std::vector<PanelMenuEntry> BuildPanelMenu(
    std::span<const PanelDefinition> panels,
    std::span<const u8> panelOpen);

class EditorUi
{
public:
    // `uiScale` is the window's DPI scale (Window::DpiScale(): 1.0 at 100%,
    // 2.0 at Windows' 200%); it sizes the loaded font and every style metric
    // (padding, rounding, scrollbar/grab size) so the editor stays legible on
    // a high-DPI display instead of rendering pixel-perfect-but-tiny.
    EditorUi(
        rhi::Device& device,
        rhi::Queue& graphicsQueue,
        const shader::Compiler& compiler,
        std::filesystem::path layoutPath,
        f32 uiScale = 1.0F);

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
    [[nodiscard]] bool PanelVisible(PanelId id) const noexcept;
    // Opens the panel if needed and brings it to the front of its tab group.
    [[nodiscard]] bool FocusPanel(PanelId id) noexcept;

    // Automation surface (studio.panel_* RPC): every panel with its state,
    // and title-based open/focus/close so an agent can reach any tab.
    struct PanelSummary
    {
        PanelId id{};
        std::string title;
        bool open{false};
        bool visible{false};
        DockRegion region{DockRegion::Auto};
    };
    [[nodiscard]] std::vector<PanelSummary> Panels() const;
    // Case-insensitive title match. False when no panel has that title.
    [[nodiscard]] bool FocusPanelByTitle(std::string_view title) noexcept;
    [[nodiscard]] bool ClosePanelByTitle(std::string_view title) noexcept;

    // Reuses an already-registered panel's presentation body inside the
    // caller's current panel without changing the source panel's docking/open
    // state. Composite Studio surfaces therefore keep one authoritative set of
    // callbacks for selection, drag/drop, thumbnails and context actions.
    // Recursive composition is rejected and returns false.
    [[nodiscard]] bool DrawPanelContentsByTitle(
        std::string_view title,
        PanelContext& context);

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
    // Work area available to docked panels after the main menu and persistent
    // shell bands reserve their space, in the same logical pixels as
    // AutomationPanelLayout.
    [[nodiscard]] UiSize AutomationWorkArea() const;

    // Deterministic validation seam: keeps the named main-menu dropdown open
    // (empty string clears it) so its contents can be captured or inspected
    // without real mouse input. Normal Studio never sets this.
    void SetAutomationOpenMenu(std::string menu);

    // Discards the current arrangement and re-applies the default dock layout
    // on the next DrawStudioShell.
    void ResetLayout() noexcept;

    void RegisterMenuAction(MenuAction action);

    // Persistent "which project am I in" chip drawn at the right edge of the
    // main menu bar. An empty name hides it. Presentation only: the owner
    // refreshes it each frame from the authoritative project/world state.
    struct ProjectIndicator
    {
        std::string name;
        std::string detail;
        std::string tooltip;
        std::function<void()> onClick;
    };
    void SetProjectIndicator(ProjectIndicator indicator);

    // Transient, non-modal, click-through popup stacked in the lower-right of
    // the main viewport. Presentation only: it never gates the operation it
    // reports. Safe to call any time on the UI thread; the toast is created on
    // the next DrawStudioShell and fades out after `seconds`.
    enum class NotificationSeverity : u8
    {
        Info,
        Error
    };
    struct Notification
    {
        std::string title;
        std::string detail;
        NotificationSeverity severity{NotificationSeverity::Info};
        f32 seconds{4.0F};
    };
    void PushNotification(Notification notification);

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

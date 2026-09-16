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
    PanelContext() = default;
    friend class EditorUi;
};

struct PanelDefinition
{
    PanelId id{};
    std::string title;
    bool defaultOpen{true};
    std::function<void(PanelContext&)> draw;
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

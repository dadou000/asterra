#pragma once

#include <orbit/core/StrongId.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/platform/Window.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/rhi/Queue.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <filesystem>
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

class PanelContext
{
public:
    void Text(std::string_view text);
    void Separator();

    [[nodiscard]] bool Button(
        std::string_view label);

    [[nodiscard]] bool InputText(
        std::string_view label,
        std::string& value);

    [[nodiscard]] UiSize ContentAvailable() const;

    void Image(
        rhi::Texture& texture,
        UiSize size);

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

    void RegisterPanel(
        PanelDefinition panel);

    void RegisterMenuAction(
        MenuAction action);

    void BeginFrame(
        platform::Window& window,
        f64 deltaSeconds);

    // Creates the main dockspace, top menu/ribbon and all registered
    // panels. Panel code sees only PanelContext, never ImGui.
    void DrawStudioShell();

    // Finalizes ImGui and emits draw data through Orbit RHI into a color
    // target already transitioned to RenderTarget.
    void Render(
        rhi::CommandList& commands,
        rhi::Texture& target,
        u32 targetWidth,
        u32 targetHeight);

    [[nodiscard]] bool WantsMouse() const noexcept;
    [[nodiscard]] bool WantsKeyboard() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::editor_ui

#include <orbit/editor_ui/EditorUi.hpp>

#include <imgui.h>

#include <orbit/rhi/Pipeline.hpp>
#include <orbit/rhi/Resource.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace orbit::editor_ui
{
namespace
{
struct UiVertex
{
    math::Float2 position{};
    math::Float2 uv{};
    math::Float4 color{};
};

constexpr const char* kVertexShader = R"(
struct Push
{
    float2 scale;
    float2 translate;
};
[[vk::push_constant]] Push g_push;

struct VSInput
{
    [[vk::location(0)]] float2 position : POSITION;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float4 color : COLOR0;
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.position =
        float4(
            input.position * g_push.scale +
                g_push.translate,
            0.0,
            1.0);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}
)";

constexpr const char* kPixelShader = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

[[vk::binding(0, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_texture;

[[vk::binding(0, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_sampler;

float4 main(VSOutput input) : SV_Target0
{
    return input.color *
        g_texture.Sample(
            g_sampler,
            input.uv);
}
)";

[[nodiscard]] math::Float4 DecodeColor(
    const ImU32 packed) noexcept
{
    return {
        static_cast<f32>(
            packed & 0xFFU) /
            255.0F,
        static_cast<f32>(
            (packed >> 8U) & 0xFFU) /
            255.0F,
        static_cast<f32>(
            (packed >> 16U) & 0xFFU) /
            255.0F,
        static_cast<f32>(
            (packed >> 24U) & 0xFFU) /
            255.0F
    };
}

[[nodiscard]] ImTextureID ToTextureId(
    rhi::Texture& texture) noexcept
{
    return static_cast<ImTextureID>(
        reinterpret_cast<std::uintptr_t>(
            &texture));
}

[[nodiscard]] rhi::Texture* FromTextureId(
    const ImTextureID id) noexcept
{
    return reinterpret_cast<rhi::Texture*>(
        static_cast<std::uintptr_t>(id));
}

void FeedKey(
    ImGuiIO& io,
    const ImGuiKey imguiKey,
    platform::Window& window,
    const platform::Key key)
{
    io.AddKeyEvent(
        imguiKey,
        window.KeyDown(key));
}
} // namespace

class EditorUi::Impl
{
public:
    Impl(
        rhi::Device& device,
        rhi::Queue& graphicsQueue,
        const shader::Compiler& compiler,
        std::filesystem::path layoutPath)
        : device(device),
          graphicsQueue(graphicsQueue),
          layoutPath(
              std::move(layoutPath))
    {
        IMGUI_CHECKVERSION();
        context = ImGui::CreateContext();

        if (context == nullptr)
        {
            throw std::runtime_error(
                "Dear ImGui context creation failed.");
        }

        ImGui::SetCurrentContext(context);

        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |=
            ImGuiConfigFlags_DockingEnable;
        io.ConfigWindowsMoveFromTitleBarOnly =
            true;

        std::filesystem::create_directories(
            this->layoutPath.parent_path());

        layoutPathUtf8 =
            this->layoutPath.string();
        io.IniFilename =
            layoutPathUtf8.c_str();

        ImGui::StyleColorsDark();

        const shader::Binary vertex =
            compiler.Compile({
                .source = kVertexShader,
                .entryPoint = "main",
                .stage = shader::Stage::Vertex,
                .debug = false
            });

        const shader::Binary pixel =
            compiler.Compile({
                .source = kPixelShader,
                .entryPoint = "main",
                .stage = shader::Stage::Pixel,
                .debug = false
            });

        constexpr std::array<
            rhi::VertexAttribute,
            3> attributes{{
                {
                    .location = 0,
                    .format =
                        rhi::VertexFormat::Float2,
                    .offsetBytes =
                        offsetof(
                            UiVertex,
                            position)
                },
                {
                    .location = 1,
                    .format =
                        rhi::VertexFormat::Float2,
                    .offsetBytes =
                        offsetof(
                            UiVertex,
                            uv)
                },
                {
                    .location = 2,
                    .format =
                        rhi::VertexFormat::Float4,
                    .offsetBytes =
                        offsetof(
                            UiVertex,
                            color)
                }
            }};

        pipeline =
            device.CreateGraphicsPipeline({
                .vertexShader = {
                    .data =
                        vertex.bytecode.data(),
                    .size =
                        vertex.bytecode.size()
                },
                .pixelShader = {
                    .data =
                        pixel.bytecode.data(),
                    .size =
                        pixel.bytecode.size()
                },
                .vertexAttributes =
                    attributes,
                .vertexStrideBytes =
                    sizeof(UiVertex),
                .pushConstantDwords = 4,
                .shaderResourceBuffers = 0,
                .sampledTextures = 1,
                .topology =
                    rhi::PrimitiveTopology::
                        TriangleList,
                .fillMode =
                    rhi::FillMode::Solid,
                .cullMode =
                    rhi::CullMode::None,
                .blendMode =
                    rhi::BlendMode::Alpha,
                .depthTest = false,
                .depthWrite = false
            });

        UploadFontAtlas();
    }

    ~Impl()
    {
        if (context != nullptr)
        {
            ImGui::SetCurrentContext(
                context);
            ImGui::SaveIniSettingsToDisk(
                layoutPathUtf8.c_str());
            ImGui::DestroyContext(
                context);
        }
    }

    void UploadFontAtlas()
    {
        ImGui::SetCurrentContext(context);

        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;

        ImGui::GetIO().Fonts->
            GetTexDataAsRGBA32(
                &pixels,
                &width,
                &height);

        if (pixels == nullptr ||
            width <= 0 ||
            height <= 0)
        {
            throw std::runtime_error(
                "Dear ImGui font atlas generation failed.");
        }

        const u64 byteCount =
            static_cast<u64>(width) *
            static_cast<u64>(height) *
            4ULL;

        fontTexture =
            device.CreateTexture({
                .width =
                    static_cast<u32>(
                        width),
                .height =
                    static_cast<u32>(
                        height),
                .format =
                    rhi::TextureFormat::
                        RGBA8_UNorm,
                .initialState =
                    rhi::ResourceState::
                        Common
            });

        auto staging =
            device.CreateBuffer({
                .sizeBytes = byteCount,
                .usage =
                    rhi::BufferUsage::Generic,
                .memory =
                    rhi::MemoryUsage::
                        HostVisible,
                .initialState =
                    rhi::ResourceState::
                        Common
            });

        std::memcpy(
            staging->Map(),
            pixels,
            static_cast<std::size_t>(
                byteCount));
        staging->Unmap();

        auto allocator =
            device.CreateCommandAllocator(
                rhi::QueueType::Graphics);

        auto commands =
            device.CreateCommandList(
                *allocator);

        auto fence =
            device.CreateFence(0);

        commands->Reset(*allocator);
        commands->Transition(
            *fontTexture,
            rhi::ResourceState::Common,
            rhi::ResourceState::
                CopyDestination);

        commands->CopyBufferToTexture(
            *staging,
            0,
            *fontTexture);

        commands->Transition(
            *fontTexture,
            rhi::ResourceState::
                CopyDestination,
            rhi::ResourceState::
                ShaderResource);

        commands->Close();
        graphicsQueue.Submit(
            *commands);
        graphicsQueue.Signal(
            *fence,
            1);
        fence->Wait(1);

        ImGui::GetIO().Fonts->SetTexID(
            ToTextureId(
                *fontTexture));
    }

    void EnsureBuffers(
        const std::size_t vertexCount,
        const std::size_t indexCount)
    {
        if (vertexCount >
            vertexCapacity)
        {
            vertexCapacity =
                std::max<std::size_t>(
                    vertexCount,
                    vertexCapacity *
                            2U +
                        4096U);

            vertexBuffer =
                device.CreateBuffer({
                    .sizeBytes =
                        static_cast<u64>(
                            vertexCapacity *
                            sizeof(UiVertex)),
                    .usage =
                        rhi::BufferUsage::Vertex,
                    .memory =
                        rhi::MemoryUsage::
                            HostVisible,
                    .initialState =
                        rhi::ResourceState::
                            VertexOrConstantBuffer
                });
        }

        if (indexCount >
            indexCapacity)
        {
            indexCapacity =
                std::max<std::size_t>(
                    indexCount,
                    indexCapacity *
                            2U +
                        8192U);

            indexBuffer =
                device.CreateBuffer({
                    .sizeBytes =
                        static_cast<u64>(
                            indexCapacity *
                            sizeof(u32)),
                    .usage =
                        rhi::BufferUsage::Index,
                    .memory =
                        rhi::MemoryUsage::
                            HostVisible,
                    .initialState =
                        rhi::ResourceState::
                            IndexBuffer
                });
        }
    }

    rhi::Device& device;
    rhi::Queue& graphicsQueue;
    std::filesystem::path layoutPath;
    std::string layoutPathUtf8;
    ImGuiContext* context{nullptr};

    std::vector<PanelDefinition> panels;
    std::vector<bool> panelOpen;
    std::vector<MenuAction> menuActions;

    std::unique_ptr<rhi::GraphicsPipeline>
        pipeline;
    std::unique_ptr<rhi::Texture>
        fontTexture;
    std::unique_ptr<rhi::Buffer>
        vertexBuffer;
    std::unique_ptr<rhi::Buffer>
        indexBuffer;
    std::size_t vertexCapacity{0};
    std::size_t indexCapacity{0};
    std::vector<UiVertex> convertedVertices;
    std::vector<u32> convertedIndices;
    bool frameBegun{false};
};

void PanelContext::Text(
    const std::string_view text)
{
    ImGui::TextUnformatted(
        text.data(),
        text.data() + text.size());
}

void PanelContext::Separator()
{
    ImGui::Separator();
}

bool PanelContext::Button(
    const std::string_view label)
{
    const std::string owned(label);
    return ImGui::Button(
        owned.c_str());
}

bool PanelContext::InputText(
    const std::string_view label,
    std::string& value)
{
    const std::string ownedLabel(
        label);

    const std::size_t minimumCapacity =
        std::max<std::size_t>(
            value.size() + 256U,
            512U);

    std::vector<char> buffer(
        minimumCapacity,
        '\0');

    std::memcpy(
        buffer.data(),
        value.data(),
        value.size());

    if (!ImGui::InputText(
            ownedLabel.c_str(),
            buffer.data(),
            buffer.size()))
    {
        return false;
    }

    value.assign(
        buffer.data());
    return true;
}

UiSize PanelContext::ContentAvailable() const
{
    const ImVec2 size =
        ImGui::GetContentRegionAvail();

    return {
        .width =
            std::max(size.x, 0.0F),
        .height =
            std::max(size.y, 0.0F)
    };
}

void PanelContext::Image(
    rhi::Texture& texture,
    const UiSize size)
{
    ImGui::Image(
        ToTextureId(texture),
        ImVec2(
            size.width,
            size.height));
}

void PanelContext::SameLine()
{
    ImGui::SameLine();
}

EditorUi::EditorUi(
    rhi::Device& device,
    rhi::Queue& graphicsQueue,
    const shader::Compiler& compiler,
    std::filesystem::path layoutPath)
    : impl_(
          std::make_unique<Impl>(
              device,
              graphicsQueue,
              compiler,
              std::move(layoutPath)))
{
}

EditorUi::~EditorUi() = default;

void EditorUi::RegisterPanel(
    PanelDefinition panel)
{
    if (!panel.id ||
        panel.title.empty() ||
        !panel.draw)
    {
        throw std::invalid_argument(
            "Editor panel requires stable ID, title and draw callback.");
    }

    for (const PanelDefinition& existing :
         impl_->panels)
    {
        if (existing.id == panel.id)
        {
            throw std::invalid_argument(
                "Editor panel ID already registered.");
        }
    }

    impl_->panelOpen.push_back(
        panel.defaultOpen);
    impl_->panels.push_back(
        std::move(panel));
}

void EditorUi::RegisterMenuAction(
    MenuAction action)
{
    if (action.menu.empty() ||
        action.label.empty() ||
        !action.invoke)
    {
        throw std::invalid_argument(
            "Editor menu action requires menu, label and callback.");
    }

    impl_->menuActions.push_back(
        std::move(action));
}

void EditorUi::BeginFrame(
    platform::Window& window,
    const f64 deltaSeconds)
{
    if (impl_->frameBegun)
    {
        throw std::logic_error(
            "Editor UI frame already begun.");
    }

    ImGui::SetCurrentContext(
        impl_->context);

    ImGuiIO& io =
        ImGui::GetIO();

    io.DisplaySize = ImVec2(
        static_cast<f32>(
            window.Width()),
        static_cast<f32>(
            window.Height()));

    io.DeltaTime =
        static_cast<f32>(
            std::max(
                deltaSeconds,
                1.0 / 1000.0));

    const math::Double2 cursor =
        window.CursorPositionPixels();

    io.AddMousePosEvent(
        static_cast<f32>(cursor.x),
        static_cast<f32>(cursor.y));

    io.AddMouseButtonEvent(
        0,
        window.MouseButtonDown(
            platform::MouseButton::Left));

    io.AddMouseButtonEvent(
        1,
        window.MouseButtonDown(
            platform::MouseButton::Right));

    io.AddMouseButtonEvent(
        2,
        window.MouseButtonDown(
            platform::MouseButton::Middle));

    const f32 wheel =
        window.ConsumeMouseWheelDelta();

    if (wheel != 0.0F)
    {
        io.AddMouseWheelEvent(
            0.0F,
            wheel);
    }

    for (const char16_t character :
         window.ConsumeTextInputUtf16())
    {
        io.AddInputCharacterUTF16(
            static_cast<ImWchar16>(
                character));
    }

    FeedKey(io, ImGuiKey_Tab, window, platform::Key::Tab);
    FeedKey(io, ImGuiKey_LeftArrow, window, platform::Key::ArrowLeft);
    FeedKey(io, ImGuiKey_RightArrow, window, platform::Key::ArrowRight);
    FeedKey(io, ImGuiKey_UpArrow, window, platform::Key::ArrowUp);
    FeedKey(io, ImGuiKey_DownArrow, window, platform::Key::ArrowDown);
    FeedKey(io, ImGuiKey_PageUp, window, platform::Key::PageUp);
    FeedKey(io, ImGuiKey_PageDown, window, platform::Key::PageDown);
    FeedKey(io, ImGuiKey_Home, window, platform::Key::Home);
    FeedKey(io, ImGuiKey_End, window, platform::Key::End);
    FeedKey(io, ImGuiKey_Insert, window, platform::Key::Insert);
    FeedKey(io, ImGuiKey_Delete, window, platform::Key::Delete);
    FeedKey(io, ImGuiKey_Backspace, window, platform::Key::Backspace);
    FeedKey(io, ImGuiKey_Space, window, platform::Key::Space);
    FeedKey(io, ImGuiKey_Enter, window, platform::Key::Enter);
    FeedKey(io, ImGuiKey_Escape, window, platform::Key::Escape);
    FeedKey(io, ImGuiKey_A, window, platform::Key::A);
    FeedKey(io, ImGuiKey_C, window, platform::Key::C);
    FeedKey(io, ImGuiKey_V, window, platform::Key::V);
    FeedKey(io, ImGuiKey_X, window, platform::Key::X);
    FeedKey(io, ImGuiKey_Y, window, platform::Key::Y);
    FeedKey(io, ImGuiKey_Z, window, platform::Key::Z);

    const bool control =
        window.KeyDown(
            platform::Key::LeftControl);
    const bool shift =
        window.KeyDown(
            platform::Key::LeftShift);
    const bool alt =
        window.KeyDown(
            platform::Key::LeftAlt);

    io.AddKeyEvent(
        ImGuiMod_Ctrl,
        control);
    io.AddKeyEvent(
        ImGuiMod_Shift,
        shift);
    io.AddKeyEvent(
        ImGuiMod_Alt,
        alt);

    ImGui::NewFrame();
    impl_->frameBegun = true;
}

void EditorUi::DrawStudioShell()
{
    if (!impl_->frameBegun)
    {
        throw std::logic_error(
            "Editor UI shell drawn outside a frame.");
    }

    ImGui::SetCurrentContext(
        impl_->context);

    ImGui::DockSpaceOverViewport(
        0,
        nullptr,
        ImGuiDockNodeFlags_None);

    static constexpr std::array<
        const char*,
        10> menus{
            "File",
            "Home",
            "Model",
            "World",
            "Path",
            "Material",
            "Play",
            "Build",
            "Plugins",
            "Help"
        };

    if (ImGui::BeginMainMenuBar())
    {
        for (const char* menu : menus)
        {
            if (!ImGui::BeginMenu(menu))
            {
                continue;
            }

            bool emitted = false;

            for (const MenuAction& action :
                 impl_->menuActions)
            {
                if (action.menu != menu)
                {
                    continue;
                }

                emitted = true;

                const bool enabled =
                    !action.enabled ||
                    action.enabled();

                if (ImGui::MenuItem(
                        action.label.c_str(),
                        nullptr,
                        false,
                        enabled))
                {
                    action.invoke();
                }
            }

            if (!emitted)
            {
                ImGui::TextDisabled(
                    "No actions");
            }

            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }

    PanelContext context;

    for (std::size_t index = 0;
         index < impl_->panels.size();
         ++index)
    {
        if (!impl_->panelOpen[index])
        {
            continue;
        }

        PanelDefinition& panel =
            impl_->panels[index];

        if (ImGui::Begin(
                panel.title.c_str(),
                &impl_->panelOpen[index]))
        {
            panel.draw(context);
        }

        ImGui::End();
    }
}

void EditorUi::Render(
    rhi::CommandList& commands,
    rhi::Texture& target,
    const u32 targetWidth,
    const u32 targetHeight)
{
    if (!impl_->frameBegun)
    {
        throw std::logic_error(
            "Editor UI render requested before BeginFrame.");
    }

    ImGui::SetCurrentContext(
        impl_->context);
    ImGui::Render();
    impl_->frameBegun = false;

    ImDrawData* drawData =
        ImGui::GetDrawData();

    if (drawData == nullptr ||
        drawData->TotalVtxCount <= 0 ||
        drawData->TotalIdxCount <= 0 ||
        targetWidth == 0 ||
        targetHeight == 0)
    {
        return;
    }

    impl_->EnsureBuffers(
        static_cast<std::size_t>(
            drawData->TotalVtxCount),
        static_cast<std::size_t>(
            drawData->TotalIdxCount));

    impl_->convertedVertices.clear();
    impl_->convertedIndices.clear();

    impl_->convertedVertices.reserve(
        static_cast<std::size_t>(
            drawData->TotalVtxCount));

    impl_->convertedIndices.reserve(
        static_cast<std::size_t>(
            drawData->TotalIdxCount));

    u32 vertexBase = 0;

    for (int listIndex = 0;
         listIndex < drawData->CmdListsCount;
         ++listIndex)
    {
        const ImDrawList* list =
            drawData->CmdLists[listIndex];

        for (const ImDrawVert& vertex :
             list->VtxBuffer)
        {
            impl_->convertedVertices.push_back({
                .position = {
                    vertex.pos.x,
                    vertex.pos.y
                },
                .uv = {
                    vertex.uv.x,
                    vertex.uv.y
                },
                .color =
                    DecodeColor(
                        vertex.col)
            });
        }

        for (const ImDrawIdx index :
             list->IdxBuffer)
        {
            impl_->convertedIndices.push_back(
                vertexBase +
                static_cast<u32>(index));
        }

        vertexBase +=
            static_cast<u32>(
                list->VtxBuffer.Size);
    }

    std::memcpy(
        impl_->vertexBuffer->Map(),
        impl_->convertedVertices.data(),
        impl_->convertedVertices.size() *
            sizeof(UiVertex));
    impl_->vertexBuffer->Unmap();

    std::memcpy(
        impl_->indexBuffer->Map(),
        impl_->convertedIndices.data(),
        impl_->convertedIndices.size() *
            sizeof(u32));
    impl_->indexBuffer->Unmap();

    commands.SetRenderTarget(target);
    commands.SetViewport({
        .x = 0.0F,
        .y = 0.0F,
        .width =
            static_cast<f32>(
                targetWidth),
        .height =
            static_cast<f32>(
                targetHeight),
        .minDepth = 0.0F,
        .maxDepth = 1.0F
    });

    commands.SetGraphicsPipeline(
        *impl_->pipeline);
    commands.SetVertexBuffer(
        *impl_->vertexBuffer,
        sizeof(UiVertex));
    commands.SetIndexBuffer(
        *impl_->indexBuffer,
        rhi::IndexFormat::UInt32);

    const f32 displayWidth =
        drawData->DisplaySize.x;
    const f32 displayHeight =
        drawData->DisplaySize.y;

    const std::array<u32, 4>
        constants{
            std::bit_cast<u32>(
                2.0F /
                displayWidth),
            std::bit_cast<u32>(
                2.0F /
                displayHeight),
            std::bit_cast<u32>(
                -1.0F -
                drawData->DisplayPos.x *
                    (2.0F /
                     displayWidth)),
            std::bit_cast<u32>(
                -1.0F -
                drawData->DisplayPos.y *
                    (2.0F /
                     displayHeight))
        };

    commands.SetGraphicsConstants(
        constants);

    u32 globalIndexOffset = 0;
    u32 globalVertexOffset = 0;

    for (int listIndex = 0;
         listIndex < drawData->CmdListsCount;
         ++listIndex)
    {
        const ImDrawList* list =
            drawData->CmdLists[listIndex];

        for (const ImDrawCmd& draw :
             list->CmdBuffer)
        {
            if (draw.UserCallback != nullptr)
            {
                if (draw.UserCallback !=
                    ImDrawCallback_ResetRenderState)
                {
                    draw.UserCallback(
                        list,
                        &draw);
                }

                continue;
            }

            const ImVec2 clipMin{
                (draw.ClipRect.x -
                 drawData->DisplayPos.x) *
                    drawData->
                        FramebufferScale.x,
                (draw.ClipRect.y -
                 drawData->DisplayPos.y) *
                    drawData->
                        FramebufferScale.y
            };

            const ImVec2 clipMax{
                (draw.ClipRect.z -
                 drawData->DisplayPos.x) *
                    drawData->
                        FramebufferScale.x,
                (draw.ClipRect.w -
                 drawData->DisplayPos.y) *
                    drawData->
                        FramebufferScale.y
            };

            const i32 left =
                std::max(
                    static_cast<i32>(
                        std::floor(
                            clipMin.x)),
                    0);

            const i32 top =
                std::max(
                    static_cast<i32>(
                        std::floor(
                            clipMin.y)),
                    0);

            const i32 right =
                std::min(
                    static_cast<i32>(
                        std::ceil(
                            clipMax.x)),
                    static_cast<i32>(
                        targetWidth));

            const i32 bottom =
                std::min(
                    static_cast<i32>(
                        std::ceil(
                            clipMax.y)),
                    static_cast<i32>(
                        targetHeight));

            if (right <= left ||
                bottom <= top)
            {
                continue;
            }

            commands.SetScissor({
                .left = left,
                .top = top,
                .right = right,
                .bottom = bottom
            });

            rhi::Texture* texture =
                FromTextureId(
                    draw.GetTexID());

            if (texture == nullptr)
            {
                texture =
                    impl_->fontTexture.get();
            }

            commands.SetGraphicsTexture(
                0,
                *texture);

            commands.DrawIndexed(
                draw.ElemCount,
                globalIndexOffset +
                    draw.IdxOffset,
                static_cast<i32>(
                    globalVertexOffset +
                    draw.VtxOffset));
        }

        globalIndexOffset +=
            static_cast<u32>(
                list->IdxBuffer.Size);

        globalVertexOffset +=
            static_cast<u32>(
                list->VtxBuffer.Size);
    }
}

bool EditorUi::WantsMouse() const noexcept
{
    ImGui::SetCurrentContext(
        impl_->context);
    return ImGui::GetIO().
        WantCaptureMouse;
}

bool EditorUi::WantsKeyboard() const noexcept
{
    ImGui::SetCurrentContext(
        impl_->context);
    return ImGui::GetIO().
        WantCaptureKeyboard;
}
} // namespace orbit::editor_ui

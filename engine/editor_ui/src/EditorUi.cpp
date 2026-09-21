#include <orbit/editor_ui/EditorUi.hpp>

#include <imgui.h>
#include <imgui_internal.h>

#include <orbit/rhi/Pipeline.hpp>
#include <orbit/rhi/Resource.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
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

struct UiProjection
{
    f32 scaleX{};
    f32 scaleY{};
    f32 translateX{};
    f32 translateY{};
};

[[nodiscard]] constexpr UiProjection MakeUiProjection(
    const f32 displayPositionX,
    const f32 displayPositionY,
    const f32 displayWidth,
    const f32 displayHeight) noexcept
{
    return {
        .scaleX = 2.0F / displayWidth,
        .scaleY = -2.0F / displayHeight,
        .translateX =
            -1.0F -
            displayPositionX *
                (2.0F / displayWidth),
        .translateY =
            1.0F +
            displayPositionY *
                (2.0F / displayHeight)
    };
}

constexpr UiProjection kProjectionContract =
    MakeUiProjection(
        0.0F,
        0.0F,
        100.0F,
        100.0F);

static_assert(
    kProjectionContract.translateY > 0.99F &&
    100.0F * kProjectionContract.scaleY +
            kProjectionContract.translateY <
        -0.99F,
    "Editor UI must map top-left draw coordinates to the top of Orbit's D3D-style Vulkan viewport.");

[[nodiscard]] std::string ReadLayoutText(
    const std::filesystem::path& path)
{
    std::ifstream stream(
        path,
        std::ios::binary);

    if (!stream)
    {
        return {};
    }

    return std::string(
        std::istreambuf_iterator<char>(
            stream),
        std::istreambuf_iterator<char>());
}

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


// Studio's look follows the Wavelength lighting controller: a deep navy
// canvas, translucent glass-like panels, pill tabs, rounded controls and a
// single royal-blue accent. Every colour below comes from that app's design
// tokens (public/styles.css) so the two stay recognisably one family.
[[nodiscard]] constexpr ImVec4 Rgb(
    const int red,
    const int green,
    const int blue,
    const f32 alpha = 1.0F) noexcept
{
    return ImVec4(
        static_cast<f32>(red) / 255.0F,
        static_cast<f32>(green) / 255.0F,
        static_cast<f32>(blue) / 255.0F,
        alpha);
}

[[nodiscard]] constexpr ImVec4 White(
    const f32 alpha) noexcept
{
    return ImVec4(1.0F, 1.0F, 1.0F, alpha);
}

namespace wavelength
{
// Canvas: radial-gradient(circle at top, #11172c, #06070f 65%).
constexpr ImVec4 kCanvasTop = Rgb(0x11, 0x17, 0x2c);
constexpr ImVec4 kCanvasBase = Rgb(0x06, 0x07, 0x0f);

constexpr ImVec4 kText = Rgb(0xf2, 0xf5, 0xff);
constexpr ImVec4 kTextHeading = Rgb(0xf8, 0xfa, 0xfc);
constexpr ImVec4 kTextMuted = Rgb(0x9e, 0xa7, 0xc3);
constexpr ImVec4 kTextSoft = Rgb(0x7d, 0x84, 0xa8);

constexpr ImVec4 kAccent = Rgb(0x4b, 0x7b, 0xec);
constexpr ImVec4 kAccentHover = Rgb(0x6b, 0x93, 0xf2);
constexpr ImVec4 kAccentPressed = Rgb(0x3c, 0x5a, 0xdc);
constexpr ImVec4 kAccentLight = Rgb(0x8e, 0xa2, 0xff);

constexpr ImVec4 kPanel = Rgb(14, 18, 33, 0.88F);
constexpr ImVec4 kCard = Rgb(17, 22, 39, 0.92F);
constexpr ImVec4 kModal = Rgb(11, 15, 27, 0.98F);
constexpr ImVec4 kBorder = White(0.08F);
constexpr ImVec4 kBorderStrong = White(0.14F);
} // namespace wavelength

void ApplyWavelengthTheme()
{
    using namespace wavelength;

    ImGuiStyle& style = ImGui::GetStyle();

    style.WindowPadding = ImVec2(14.0F, 12.0F);
    style.FramePadding = ImVec2(12.0F, 6.0F);
    style.CellPadding = ImVec2(8.0F, 5.0F);
    style.ItemSpacing = ImVec2(10.0F, 8.0F);
    style.ItemInnerSpacing = ImVec2(8.0F, 5.0F);
    style.TouchExtraPadding = ImVec2(0.0F, 0.0F);
    style.IndentSpacing = 20.0F;
    style.ScrollbarSize = 12.0F;
    style.GrabMinSize = 14.0F;
    // A visible gap between docked panels lets the canvas show through, so
    // panels read as separate rounded cards as they do in Wavelength.
    style.DockingSeparatorSize = 6.0F;

    style.WindowBorderSize = 1.0F;
    style.ChildBorderSize = 1.0F;
    style.PopupBorderSize = 1.0F;
    style.FrameBorderSize = 1.0F;
    style.TabBorderSize = 0.0F;

    style.WindowRounding = 14.0F;
    style.ChildRounding = 12.0F;
    style.FrameRounding = 10.0F;
    style.PopupRounding = 14.0F;
    style.ScrollbarRounding = 12.0F;
    style.GrabRounding = 10.0F;
    style.TabRounding = 12.0F;

    style.WindowMenuButtonPosition = ImGuiDir_Right;
    style.ColorButtonPosition = ImGuiDir_Right;
    style.ButtonTextAlign = ImVec2(0.5F, 0.5F);
    style.SelectableTextAlign = ImVec2(0.0F, 0.5F);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                 = kText;
    colors[ImGuiCol_TextDisabled]         = kTextSoft;
    colors[ImGuiCol_WindowBg]             = kPanel;
    colors[ImGuiCol_ChildBg]              = Rgb(0, 0, 0, 0.30F);
    colors[ImGuiCol_PopupBg]              = kModal;
    colors[ImGuiCol_Border]               = kBorder;
    colors[ImGuiCol_BorderShadow]         = Rgb(0, 0, 0, 0.0F);
    colors[ImGuiCol_FrameBg]              = White(0.05F);
    colors[ImGuiCol_FrameBgHovered]       = White(0.09F);
    colors[ImGuiCol_FrameBgActive]        = Rgb(0x4b, 0x7b, 0xec, 0.22F);
    colors[ImGuiCol_TitleBg]              = kCard;
    colors[ImGuiCol_TitleBgActive]        = kCard;
    colors[ImGuiCol_TitleBgCollapsed]     = kCard;
    colors[ImGuiCol_MenuBarBg]            = kPanel;
    colors[ImGuiCol_ScrollbarBg]          = Rgb(5, 7, 15, 0.80F);
    colors[ImGuiCol_ScrollbarGrab]        = Rgb(0x8e, 0xa2, 0xff, 0.55F);
    colors[ImGuiCol_ScrollbarGrabHovered] = Rgb(0x8e, 0xa2, 0xff, 0.80F);
    colors[ImGuiCol_ScrollbarGrabActive]  = kAccentLight;
    colors[ImGuiCol_CheckMark]            = kAccentLight;
    colors[ImGuiCol_SliderGrab]           = Rgb(255, 255, 255, 0.95F);
    colors[ImGuiCol_SliderGrabActive]     = kAccentLight;
    colors[ImGuiCol_Button]               = White(0.12F);
    colors[ImGuiCol_ButtonHovered]        = Rgb(0x4b, 0x7b, 0xec, 0.45F);
    colors[ImGuiCol_ButtonActive]         = kAccent;
    colors[ImGuiCol_Header]               = Rgb(0x4b, 0x7b, 0xec, 0.35F);
    colors[ImGuiCol_HeaderHovered]        = Rgb(0x4b, 0x7b, 0xec, 0.22F);
    colors[ImGuiCol_HeaderActive]         = Rgb(0x4b, 0x7b, 0xec, 0.50F);
    colors[ImGuiCol_Separator]            = kBorder;
    colors[ImGuiCol_SeparatorHovered]     = kAccentLight;
    colors[ImGuiCol_SeparatorActive]      = kAccent;
    colors[ImGuiCol_ResizeGrip]           = Rgb(0x4b, 0x7b, 0xec, 0.20F);
    colors[ImGuiCol_ResizeGripHovered]    = Rgb(0x4b, 0x7b, 0xec, 0.55F);
    colors[ImGuiCol_ResizeGripActive]     = kAccent;
    colors[ImGuiCol_Tab]                  = White(0.08F);
    colors[ImGuiCol_TabHovered]           = Rgb(0x4b, 0x7b, 0xec, 0.55F);
    colors[ImGuiCol_TabActive]            = kAccent;
    colors[ImGuiCol_TabUnfocused]         = White(0.05F);
    colors[ImGuiCol_TabUnfocusedActive]   = Rgb(0x4b, 0x7b, 0xec, 0.55F);
    colors[ImGuiCol_DockingPreview]       = Rgb(0x4b, 0x7b, 0xec, 0.55F);
    colors[ImGuiCol_DockingEmptyBg]       = Rgb(0x06, 0x07, 0x0f, 0.0F);
    colors[ImGuiCol_PlotLines]            = kAccentLight;
    colors[ImGuiCol_PlotLinesHovered]     = kAccentHover;
    colors[ImGuiCol_PlotHistogram]        = kAccent;
    colors[ImGuiCol_PlotHistogramHovered] = kAccentHover;
    colors[ImGuiCol_TableHeaderBg]        = White(0.05F);
    colors[ImGuiCol_TableBorderStrong]    = kBorderStrong;
    colors[ImGuiCol_TableBorderLight]     = kBorder;
    colors[ImGuiCol_TableRowBg]           = Rgb(0, 0, 0, 0.0F);
    colors[ImGuiCol_TableRowBgAlt]        = White(0.025F);
    colors[ImGuiCol_TextSelectedBg]       = Rgb(0x4b, 0x7b, 0xec, 0.45F);
    colors[ImGuiCol_DragDropTarget]       = kAccentLight;
    colors[ImGuiCol_NavHighlight]         = kAccentLight;
    colors[ImGuiCol_NavWindowingHighlight]= White(0.70F);
    colors[ImGuiCol_NavWindowingDimBg]    = Rgb(0, 0, 0, 0.55F);
    colors[ImGuiCol_ModalWindowDimBg]     = Rgb(0, 0, 0, 0.65F);
}

// Wavelength uses Segoe UI; ImGui's built-in pixel font is what made the
// stock editor look like a debug overlay. Falls back to the built-in font
// when Segoe UI is not installed (non-Windows or stripped images).
void LoadStudioFont(
    ImGuiIO& io)
{
    constexpr f32 kFontPixels = 16.0F;

    // The stock Windows install location; anything else falls back below.
    const std::filesystem::path candidate =
        "C:/Windows/Fonts/segoeui.ttf";

    std::error_code ignored;

    if (std::filesystem::exists(
            candidate,
            ignored))
    {
        ImFontConfig config;
        config.OversampleH = 3;
        config.OversampleV = 2;

        if (io.Fonts->AddFontFromFileTTF(
                candidate.string().c_str(),
                kFontPixels,
                &config) != nullptr)
        {
            return;
        }
    }

    io.Fonts->AddFontDefault();
}

// radial-gradient(circle at top, #11172c, #06070f 65%) painted behind the
// dockspace. Drawn as a triangle fan so no texture is needed; the disc is
// centred on the top edge with the CSS "farthest-corner" radius.
void DrawWavelengthCanvas(
    const ImGuiViewport& viewport)
{
    ImDrawList* list =
        ImGui::GetBackgroundDrawList();

    const ImVec2 origin = viewport.Pos;
    const ImVec2 size = viewport.Size;

    list->AddRectFilled(
        origin,
        ImVec2(
            origin.x + size.x,
            origin.y + size.y),
        ImGui::GetColorU32(
            wavelength::kCanvasBase));

    const ImVec2 center(
        origin.x + size.x * 0.5F,
        origin.y);
    const f32 radius =
        0.65F *
        std::sqrt(
            (size.x * 0.5F) *
                (size.x * 0.5F) +
            size.y * size.y);

    constexpr int kSegments = 64;
    const ImU32 inner =
        ImGui::GetColorU32(
            wavelength::kCanvasTop);
    const ImU32 outer =
        ImGui::GetColorU32(
            wavelength::kCanvasBase);
    const ImVec2 white =
        ImGui::GetDrawListSharedData()->
            TexUvWhitePixel;

    list->PrimReserve(
        kSegments * 3,
        kSegments + 2);

    const ImDrawIdx base =
        static_cast<ImDrawIdx>(
            list->_VtxCurrentIdx);

    list->PrimWriteVtx(
        center,
        white,
        inner);

    for (int index = 0;
         index <= kSegments;
         ++index)
    {
        const f32 angle =
            3.14159265F *
            static_cast<f32>(index) /
            static_cast<f32>(kSegments);

        list->PrimWriteVtx(
            ImVec2(
                center.x +
                    std::cos(angle) *
                        radius,
                center.y +
                    std::sin(angle) *
                        radius),
            white,
            outer);
    }

    for (int index = 0;
         index < kSegments;
         ++index)
    {
        list->PrimWriteIdx(base);
        list->PrimWriteIdx(
            static_cast<ImDrawIdx>(
                base + 1 + index));
        list->PrimWriteIdx(
            static_cast<ImDrawIdx>(
                base + 2 + index));
    }
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

        // ImGui loads this file on the first frame. If it does not dock
        // anything (missing, or every panel floating) the default arrangement
        // is built instead of leaving panels stacked at ImGui's default
        // window position.
        layoutBuildPending =
            !LayoutTextHasDockedPanels(
                ReadLayoutText(
                    this->layoutPath));

        ImGui::StyleColorsDark();
        ApplyWavelengthTheme();
        LoadStudioFont(io);

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

    // Splits the dock space into Left | Center | Right with Bottom spanning
    // the full width, and docks every participating panel into its region.
    // Regions with no panels are not split off. The caller must be inside a
    // frame with the main dock space already submitted.
    void BuildDefaultLayout(
        const ImGuiID dockspace)
    {
        const std::vector<DockAssignment> assignments =
            AssignDefaultDock(
                panels);

        if (assignments.empty())
        {
            return;
        }

        const DockSplitPlan plan =
            PlanDockSplits(
                assignments,
                DockLayoutFractions{});

        const ImGuiViewport* viewport =
            ImGui::GetMainViewport();

        ImGui::DockBuilderRemoveNode(
            dockspace);
        ImGui::DockBuilderAddNode(
            dockspace,
            ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodePos(
            dockspace,
            viewport->WorkPos);
        ImGui::DockBuilderSetNodeSize(
            dockspace,
            viewport->WorkSize);

        ImGuiID center = dockspace;
        ImGuiID left = 0;
        ImGuiID right = 0;
        ImGuiID bottom = 0;

        if (plan.bottomOfRoot > 0.0F)
        {
            ImGui::DockBuilderSplitNode(
                center,
                ImGuiDir_Down,
                plan.bottomOfRoot,
                &bottom,
                &center);
        }

        if (plan.leftOfRemainder > 0.0F)
        {
            ImGui::DockBuilderSplitNode(
                center,
                ImGuiDir_Left,
                plan.leftOfRemainder,
                &left,
                &center);
        }

        if (plan.rightOfRemainder > 0.0F)
        {
            ImGui::DockBuilderSplitNode(
                center,
                ImGuiDir_Right,
                plan.rightOfRemainder,
                &right,
                &center);
        }

        // DockBuilderDockWindow clears each window's tab-order hint, which
        // leaves tabs in first-submission order. Restore the intended order
        // explicitly so the first assignment in a region is its first tab.
        std::array<short, 5> nextOrder{};

        for (const DockAssignment& assignment :
             assignments)
        {
            const auto panel =
                std::ranges::find_if(
                    panels,
                    [&assignment](
                        const PanelDefinition& candidate)
                    {
                        return candidate.id ==
                            assignment.panel;
                    });

            if (panel == panels.end())
            {
                continue;
            }

            ImGuiID node = center;

            switch (assignment.region)
            {
            case DockRegion::Left:
                node = left;
                break;
            case DockRegion::Right:
                node = right;
                break;
            case DockRegion::Bottom:
                node = bottom;
                break;
            case DockRegion::Center:
            case DockRegion::Auto:
                break;
            }

            ImGui::DockBuilderDockWindow(
                panel->title.c_str(),
                node);

            const short order =
                nextOrder[static_cast<std::size_t>(
                    assignment.region)]++;
            const ImGuiID windowId =
                ImHashStr(
                    panel->title.c_str());

            if (ImGuiWindow* window =
                    ImGui::FindWindowByID(
                        windowId))
            {
                window->DockOrder = order;
            }
            else if (
                ImGuiWindowSettings* settings =
                    ImGui::FindWindowSettingsByID(
                        windowId))
            {
                settings->DockOrder = order;
            }
        }

        ImGui::DockBuilderFinish(
            dockspace);
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
    std::vector<u8> panelOpen;
    std::vector<MenuAction> menuActions;
    bool automationExpandTrees{false};
    bool automationTraceWidgets{false};
    std::vector<std::string> automationTrace;
    bool layoutBuildPending{false};
    std::string pendingFocusTitle;
    std::string automationOpenMenu;

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

PanelContext::PanelContext(
    const bool forceTreeOpen,
    std::vector<std::string>* automationTrace) noexcept
    : forceTreeOpen_(forceTreeOpen),
      automationTrace_(automationTrace)
{
}

void PanelContext::TraceWidget(
    const std::string_view label) const
{
    if (automationTrace_ != nullptr)
    {
        automationTrace_->emplace_back(label);
    }
}

void PanelContext::Text(
    const std::string_view text)
{
    ImGui::TextUnformatted(
        text.data(),
        text.data() + text.size());
}

void PanelContext::MutedText(
    const std::string_view text)
{
    ImGui::PushStyleColor(
        ImGuiCol_Text,
        wavelength::kTextMuted);
    ImGui::TextWrapped(
        "%.*s",
        static_cast<int>(text.size()),
        text.data());
    ImGui::PopStyleColor();
}

void PanelContext::Heading(
    const std::string_view text)
{
    ImGui::Dummy(ImVec2(0.0F, 3.0F));
    ImGui::PushStyleColor(
        ImGuiCol_Text,
        wavelength::kTextHeading);
    ImGui::TextUnformatted(
        text.data(),
        text.data() + text.size());
    ImGui::PopStyleColor();

    const ImVec2 minimum =
        ImGui::GetCursorScreenPos();
    const f32 width =
        ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2(minimum.x, minimum.y),
        ImVec2(
            minimum.x + width,
            minimum.y + 1.0F),
        ImGui::GetColorU32(
            Rgb(0x4b, 0x7b, 0xec, 0.60F)));
    ImGui::Dummy(ImVec2(0.0F, 5.0F));
}

void PanelContext::Separator()
{
    ImGui::Separator();
}

bool PanelContext::Button(
    const std::string_view label)
{
    TraceWidget(label);
    const std::string owned(label);
    return ImGui::Button(
        owned.c_str());
}

bool PanelContext::PrimaryButton(
    const std::string_view label)
{
    TraceWidget(label);
    const std::string owned(label);

    ImGui::PushStyleColor(
        ImGuiCol_Button,
        wavelength::kAccent);
    ImGui::PushStyleColor(
        ImGuiCol_ButtonHovered,
        wavelength::kAccentHover);
    ImGui::PushStyleColor(
        ImGuiCol_ButtonActive,
        wavelength::kAccentPressed);

    const bool pressed =
        ImGui::Button(
            owned.c_str(),
            ImVec2(
                ImGui::GetContentRegionAvail().x,
                0.0F));

    ImGui::PopStyleColor(3);
    return pressed;
}

bool PanelContext::InputText(
    const std::string_view label,
    std::string& value)
{
    TraceWidget(label);
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

bool PanelContext::Selectable(
    const std::string_view label,
    const bool selected)
{
    TraceWidget(label);
    const std::string owned(label);

    return ImGui::Selectable(
        owned.c_str(),
        selected);
}

TreeItemInteraction PanelContext::TreeItem(
    const std::string_view label,
    const bool selected)
{
    TraceWidget(label);

    if (forceTreeOpen_)
    {
        ImGui::SetNextItemOpen(
            true,
            ImGuiCond_Always);
    }

    const std::string owned(label);

    ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_OpenOnArrow |
        ImGuiTreeNodeFlags_OpenOnDoubleClick |
        ImGuiTreeNodeFlags_SpanAvailWidth;

    if (selected)
    {
        flags |=
            ImGuiTreeNodeFlags_Selected;
    }

    const bool open =
        ImGui::TreeNodeEx(
            owned.c_str(),
            flags);

    return {
        .open = open,
        .clicked =
            ImGui::IsItemClicked(
                ImGuiMouseButton_Left),
        .rightClicked =
            ImGui::IsItemClicked(
                ImGuiMouseButton_Right)
    };
}

void PanelContext::TreePop()
{
    ImGui::TreePop();
}

ImageInteraction PanelContext::Image(
    rhi::Texture& texture,
    const UiSize size)
{
    ImGui::Image(
        ToTextureId(texture),
        ImVec2(
            size.width,
            size.height));

    const bool hovered =
        ImGui::IsItemHovered();

    const bool clicked =
        ImGui::IsItemClicked(
            ImGuiMouseButton_Left);

    const bool doubleClicked =
        hovered &&
        ImGui::IsMouseDoubleClicked(
            ImGuiMouseButton_Left);

    const bool rightClicked =
        ImGui::IsItemClicked(
            ImGuiMouseButton_Right);

    f32 u = 0.0F;
    f32 v = 0.0F;

    if (hovered &&
        size.width > 0.0F &&
        size.height > 0.0F)
    {
        const ImVec2 minimum =
            ImGui::GetItemRectMin();

        const ImVec2 mouse =
            ImGui::GetMousePos();

        u = std::clamp(
            (mouse.x - minimum.x) /
                size.width,
            0.0F,
            1.0F);

        v = std::clamp(
            (mouse.y - minimum.y) /
                size.height,
            0.0F,
            1.0F);
    }

    return {
        .hovered = hovered,
        .clicked = clicked,
        .doubleClicked = doubleClicked,
        .rightClicked = rightClicked,
        .u = u,
        .v = v
    };
}

CanvasInteraction PanelContext::Canvas(
    const std::string_view id,
    const UiSize size)
{
    TraceWidget(id);

    const std::string ownedId(id);
    const ImVec2 origin =
        ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton(
        ownedId.c_str(),
        ImVec2(
            std::max(size.width, 1.0F),
            std::max(size.height, 1.0F)));

    const ImVec2 maximum{
        origin.x + std::max(size.width, 1.0F),
        origin.y + std::max(size.height, 1.0F)
    };

    ImGui::GetWindowDrawList()->AddRectFilled(
        origin,
        maximum,
        ImGui::GetColorU32(
            ImVec4(0.025F, 0.032F, 0.043F, 1.0F)),
        4.0F);

    ImGui::GetWindowDrawList()->AddRect(
        origin,
        maximum,
        ImGui::GetColorU32(
            ImVec4(0.15F, 0.20F, 0.25F, 1.0F)),
        4.0F);

    canvasOrigin_ = {
        origin.x,
        origin.y
    };
    canvasSize_ = size;
    canvasActive_ = true;

    const bool hovered =
        ImGui::IsItemHovered();

    f32 u = 0.0F;
    f32 v = 0.0F;

    if (hovered &&
        size.width > 0.0F &&
        size.height > 0.0F)
    {
        const ImVec2 mouse =
            ImGui::GetMousePos();

        u = std::clamp(
            (mouse.x - origin.x) /
                size.width,
            0.0F,
            1.0F);
        v = std::clamp(
            (mouse.y - origin.y) /
                size.height,
            0.0F,
            1.0F);
    }

    return {
        .hovered = hovered,
        .clicked =
            ImGui::IsItemClicked(
                ImGuiMouseButton_Left),
        .doubleClicked =
            hovered &&
            ImGui::IsMouseDoubleClicked(
                ImGuiMouseButton_Left),
        .rightClicked =
            ImGui::IsItemClicked(
                ImGuiMouseButton_Right),
        .dragging =
            hovered &&
            ImGui::IsMouseDragging(
                ImGuiMouseButton_Left,
                1.0F),
        .leftDown =
            hovered &&
            ImGui::IsMouseDown(
                ImGuiMouseButton_Left),
        .leftReleased =
            ImGui::IsMouseReleased(
                ImGuiMouseButton_Left),
        .u = u,
        .v = v
    };
}

void PanelContext::CanvasLine(
    const math::Float2 a,
    const math::Float2 b,
    const math::Float4 color,
    const f32 thickness)
{
    if (!canvasActive_)
    {
        return;
    }

    const auto map =
        [this](const math::Float2 p)
        {
            return ImVec2(
                canvasOrigin_.x +
                    p.x * canvasSize_.width,
                canvasOrigin_.y +
                    p.y * canvasSize_.height);
        };

    ImGui::GetWindowDrawList()->AddLine(
        map(a),
        map(b),
        ImGui::ColorConvertFloat4ToU32(
            ImVec4(
                color.x,
                color.y,
                color.z,
                color.w)),
        thickness);
}

void PanelContext::CanvasCircle(
    const math::Float2 center,
    const f32 radiusPixels,
    const math::Float4 color,
    const bool filled,
    const f32 thickness)
{
    if (!canvasActive_)
    {
        return;
    }

    const ImVec2 mapped{
        canvasOrigin_.x +
            center.x * canvasSize_.width,
        canvasOrigin_.y +
            center.y * canvasSize_.height
    };

    const ImU32 packed =
        ImGui::ColorConvertFloat4ToU32(
            ImVec4(
                color.x,
                color.y,
                color.z,
                color.w));

    if (filled)
    {
        ImGui::GetWindowDrawList()->AddCircleFilled(
            mapped,
            radiusPixels,
            packed,
            24);
    }
    else
    {
        ImGui::GetWindowDrawList()->AddCircle(
            mapped,
            radiusPixels,
            packed,
            24,
            thickness);
    }
}

void PanelContext::CanvasText(
    const math::Float2 position,
    const math::Float4 color,
    const std::string_view text)
{
    if (!canvasActive_)
    {
        return;
    }

    const ImVec2 mapped{
        canvasOrigin_.x +
            position.x * canvasSize_.width,
        canvasOrigin_.y +
            position.y * canvasSize_.height
    };

    ImGui::GetWindowDrawList()->AddText(
        mapped,
        ImGui::ColorConvertFloat4ToU32(
            ImVec4(
                color.x,
                color.y,
                color.z,
                color.w)),
        text.data(),
        text.data() + text.size());
}

bool PanelContext::Checkbox(
    const std::string_view label,
    bool& value)
{
    TraceWidget(label);
    const std::string owned(label);
    return ImGui::Checkbox(
        owned.c_str(),
        &value);
}

bool PanelContext::InputDouble(
    const std::string_view label,
    f64& value)
{
    TraceWidget(label);
    const std::string owned(label);

    return ImGui::InputDouble(
        owned.c_str(),
        &value,
        0.0,
        0.0,
        "%.6g");
}

bool PanelContext::InputInteger(
    const std::string_view label,
    i64& value)
{
    TraceWidget(label);
    const std::string owned(label);

    ImS64 native =
        static_cast<ImS64>(
            value);

    if (!ImGui::InputScalar(
            owned.c_str(),
            ImGuiDataType_S64,
            &native))
    {
        return false;
    }

    value =
        static_cast<i64>(
            native);

    return true;
}

bool PanelContext::InputDouble3(
    const std::string_view label,
    math::Double3& value)
{
    TraceWidget(label);
    const std::string owned(label);

    std::array<double, 3> native{
        value.x,
        value.y,
        value.z
    };

    if (!ImGui::InputScalarN(
            owned.c_str(),
            ImGuiDataType_Double,
            native.data(),
            3))
    {
        return false;
    }

    value = {
        native[0],
        native[1],
        native[2]
    };

    return true;
}

bool PanelContext::ControlDown() const noexcept
{
    return ImGui::GetIO().KeyCtrl;
}

bool PanelContext::BeginDragSource()
{
    return ImGui::BeginDragDropSource(
        ImGuiDragDropFlags_SourceAllowNullID);
}

void PanelContext::SetDragPayload(
    const std::string_view type,
    const std::span<const std::byte> bytes)
{
    if (type.empty())
    {
        throw std::invalid_argument(
            "Editor drag payload type must not be empty.");
    }

    const std::string ownedType(type);

    ImGui::SetDragDropPayload(
        ownedType.c_str(),
        bytes.data(),
        bytes.size());
}

void PanelContext::EndDragSource()
{
    ImGui::EndDragDropSource();
}

std::optional<std::vector<std::byte>>
PanelContext::AcceptDragPayload(
    const std::string_view type)
{
    if (type.empty())
    {
        return std::nullopt;
    }

    if (!ImGui::BeginDragDropTarget())
    {
        return std::nullopt;
    }

    const std::string ownedType(type);

    const ImGuiPayload* payload =
        ImGui::AcceptDragDropPayload(
            ownedType.c_str());

    std::optional<
        std::vector<std::byte>>
        result;

    if (payload != nullptr &&
        payload->Data != nullptr &&
        payload->DataSize > 0)
    {
        const auto* begin =
            static_cast<
                const std::byte*>(
                    payload->Data);

        result =
            std::vector<std::byte>(
                begin,
                begin +
                    payload->DataSize);
    }

    ImGui::EndDragDropTarget();
    return result;
}

void PanelContext::Toolbar(
    const std::span<const ActionPresentation> actions)
{
    bool first = true;

    for (const ActionPresentation& action : actions)
    {
        if (!first)
        {
            ImGui::SameLine();
        }

        first = false;

        if (!action.enabled)
        {
            ImGui::BeginDisabled();
        }

        const bool pressed =
            ImGui::Button(
                action.label.c_str());

        const bool hovered =
            ImGui::IsItemHovered(
                ImGuiHoveredFlags_AllowWhenDisabled);

        if (!action.enabled)
        {
            ImGui::EndDisabled();

            if (hovered &&
                !action.disabledReason.empty())
            {
                ImGui::SetTooltip(
                    "%s",
                    action.disabledReason.c_str());
            }
        }

        if (pressed &&
            action.enabled &&
            action.invoke)
        {
            action.invoke();
        }
    }
}

void PanelContext::ContextMenu(
    const std::string_view id,
    const std::span<const ActionPresentation> actions,
    const bool openRequested)
{
    const std::string ownedId(id);

    if (openRequested)
    {
        ImGui::OpenPopup(
            ownedId.c_str());
    }

    if (!ImGui::BeginPopup(
            ownedId.c_str()))
    {
        return;
    }

    for (const ActionPresentation& action : actions)
    {
        const bool selected =
            ImGui::MenuItem(
                action.label.c_str(),
                nullptr,
                false,
                action.enabled);

        if (!action.enabled &&
            ImGui::IsItemHovered(
                ImGuiHoveredFlags_AllowWhenDisabled) &&
            !action.disabledReason.empty())
        {
            ImGui::SetTooltip(
                "%s",
                action.disabledReason.c_str());
        }

        if (selected &&
            action.enabled &&
            action.invoke)
        {
            action.invoke();
        }
    }

    ImGui::EndPopup();
}

void PanelContext::RadialMenu(
    const std::string_view id,
    const std::span<const ActionPresentation> actions,
    const bool openRequested)
{
    const std::string ownedId(id);

    if (openRequested)
    {
        ImGui::OpenPopup(
            ownedId.c_str());
    }

    constexpr f32 diameter = 280.0F;
    constexpr f32 radius = 88.0F;

    ImGui::SetNextWindowSize(
        ImVec2(diameter, diameter),
        ImGuiCond_Appearing);

    if (!ImGui::BeginPopup(
            ownedId.c_str(),
            ImGuiWindowFlags_NoTitleBar |
                ImGuiWindowFlags_NoResize))
    {
        return;
    }

    const ImVec2 center{
        diameter * 0.5F,
        diameter * 0.5F
    };

    if (actions.empty())
    {
        ImGui::SetCursorPos(
            ImVec2(
                center.x - 40.0F,
                center.y - 10.0F));
        ImGui::TextDisabled(
            "No actions");
    }

    for (std::size_t index = 0;
         index < actions.size();
         ++index)
    {
        const ActionPresentation& action =
            actions[index];

        const f32 angle =
            -1.57079632679F +
            static_cast<f32>(index) *
                6.28318530718F /
                static_cast<f32>(
                    actions.size());

        const ImVec2 position{
            center.x +
                std::cos(angle) *
                    radius -
                45.0F,
            center.y +
                std::sin(angle) *
                    radius -
                14.0F
        };

        ImGui::SetCursorPos(
            position);

        if (!action.enabled)
        {
            ImGui::BeginDisabled();
        }

        const bool pressed =
            ImGui::Button(
                action.label.c_str(),
                ImVec2(90.0F, 28.0F));

        const bool hovered =
            ImGui::IsItemHovered(
                ImGuiHoveredFlags_AllowWhenDisabled);

        if (!action.enabled)
        {
            ImGui::EndDisabled();

            if (hovered &&
                !action.disabledReason.empty())
            {
                ImGui::SetTooltip(
                    "%s",
                    action.disabledReason.c_str());
            }
        }

        if (pressed &&
            action.enabled &&
            action.invoke)
        {
            action.invoke();
            ImGui::CloseCurrentPopup();
        }
    }

    ImGui::EndPopup();
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
        panel.defaultOpen ? 1U : 0U);
    impl_->panels.push_back(
        std::move(panel));
}

void EditorUi::UpsertPanel(
    PanelDefinition panel)
{
    if (!panel.id ||
        panel.title.empty() ||
        !panel.draw)
    {
        throw std::invalid_argument(
            "Editor panel requires stable ID, title and draw callback.");
    }

    for (std::size_t index = 0;
         index < impl_->panels.size();
         ++index)
    {
        if (impl_->panels[index].id ==
            panel.id)
        {
            // Preserve the live open/closed state. Plugin reload is a
            // definition update, not a request to reset the user's layout.
            impl_->panels[index] =
                std::move(panel);
            return;
        }
    }

    impl_->panelOpen.push_back(
        panel.defaultOpen ? 1U : 0U);
    impl_->panels.push_back(
        std::move(panel));
}

bool EditorUi::UnregisterPanel(
    const PanelId id)
{
    for (std::size_t index = 0;
         index < impl_->panels.size();
         ++index)
    {
        if (impl_->panels[index].id != id)
        {
            continue;
        }

        impl_->panels.erase(
            impl_->panels.begin() +
            static_cast<std::ptrdiff_t>(index));
        impl_->panelOpen.erase(
            impl_->panelOpen.begin() +
            static_cast<std::ptrdiff_t>(index));
        return true;
    }

    return false;
}

bool EditorUi::HasPanel(
    const PanelId id) const noexcept
{
    return std::ranges::any_of(
        impl_->panels,
        [id](const PanelDefinition& panel)
        {
            return panel.id == id;
        });
}

bool EditorUi::SetPanelOpen(
    const PanelId id,
    const bool open) noexcept
{
    for (std::size_t index = 0;
         index < impl_->panels.size();
         ++index)
    {
        if (impl_->panels[index].id != id)
        {
            continue;
        }

        impl_->panelOpen[index] =
            open ? 1U : 0U;
        return true;
    }

    return false;
}

bool EditorUi::PanelOpen(
    const PanelId id) const noexcept
{
    for (std::size_t index = 0;
         index < impl_->panels.size();
         ++index)
    {
        if (impl_->panels[index].id == id)
        {
            return impl_->panelOpen[index] != 0U;
        }
    }

    return false;
}

bool EditorUi::FocusPanel(
    const PanelId id) noexcept
{
    for (std::size_t index = 0;
         index < impl_->panels.size();
         ++index)
    {
        if (impl_->panels[index].id != id)
        {
            continue;
        }

        impl_->panelOpen[index] = 1U;
        impl_->pendingFocusTitle =
            impl_->panels[index].title;
        return true;
    }

    return false;
}

void EditorUi::SetAutomationUiProbe(
    const bool expandTrees,
    const bool traceWidgets) noexcept
{
    impl_->automationExpandTrees =
        expandTrees;
    impl_->automationTraceWidgets =
        traceWidgets;

    if (!traceWidgets)
    {
        impl_->automationTrace.clear();
    }
}

void EditorUi::ClearAutomationUiTrace()
{
    impl_->automationTrace.clear();
}

bool EditorUi::AutomationUiTraceContains(
    const std::string_view label) const
{
    return std::ranges::find(
               impl_->automationTrace,
               label) !=
        impl_->automationTrace.end();
}

std::size_t EditorUi::AutomationUiTraceSize() const noexcept
{
    return impl_->automationTrace.size();
}

PanelLayoutProbe EditorUi::AutomationPanelLayout(
    const PanelId id) const
{
    ImGui::SetCurrentContext(
        impl_->context);

    const auto panel =
        std::ranges::find_if(
            impl_->panels,
            [id](const PanelDefinition& candidate)
            {
                return candidate.id == id;
            });

    if (panel == impl_->panels.end())
    {
        return {};
    }

    const ImGuiWindow* window =
        ImGui::FindWindowByName(
            panel->title.c_str());

    if (window == nullptr)
    {
        return {};
    }

    return {
        .found = true,
        .docked = window->DockNode != nullptr,
        .x = window->Pos.x,
        .y = window->Pos.y,
        .width = window->Size.x,
        .height = window->Size.y
    };
}

UiSize EditorUi::AutomationWorkArea() const
{
    ImGui::SetCurrentContext(
        impl_->context);

    const ImGuiViewport* viewport =
        ImGui::GetMainViewport();

    return {
        .width = viewport->WorkSize.x,
        .height = viewport->WorkSize.y
    };
}

void EditorUi::SetAutomationOpenMenu(
    std::string menu)
{
    impl_->automationOpenMenu =
        std::move(menu);
}

void EditorUi::ResetLayout() noexcept
{
    impl_->layoutBuildPending = true;
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

    const ImGuiViewport* mainViewport =
        ImGui::GetMainViewport();

    // Wavelength-style navy radial canvas. The dockspace is pass-through, so
    // it shows in the gaps between panels and behind translucent panel glass.
    DrawWavelengthCanvas(
        *mainViewport);

    const ImGuiID mainDockspace =
        ImGui::DockSpaceOverViewport(
        0,
        nullptr,
        ImGuiDockNodeFlags_PassthruCentralNode);

    const bool hasLayoutPanels =
        !AssignDefaultDock(
             impl_->panels).
            empty();

    if (impl_->layoutBuildPending)
    {
        impl_->layoutBuildPending = false;
        impl_->BuildDefaultLayout(
            mainDockspace);
    }

    static constexpr std::array<
        const char*,
        11> menus{
            "File",
            "Home",
            "View",
            "Model",
            "World",
            "Path",
            "Material",
            "Play",
            "Build",
            "Plugins",
            "Help"
        };

    const std::vector<PanelMenuEntry> panelMenu =
        BuildPanelMenu(
            impl_->panels,
            impl_->panelOpen);

    if (ImGui::BeginMainMenuBar())
    {
        for (const char* menu : menus)
        {
            const bool isView =
                std::string_view(menu) == "View";

            const bool hasActions =
                std::ranges::any_of(
                    impl_->menuActions,
                    [menu](const MenuAction& action)
                    {
                        return action.menu == menu;
                    });

            // A menu with nothing behind it is hidden rather than shown as
            // an empty "No actions" dropdown. View is built from the panel
            // list, so it exists whenever there is a panel to toggle.
            if (isView
                    ? panelMenu.empty()
                    : !hasActions)
            {
                continue;
            }

            if (!impl_->automationOpenMenu.empty() &&
                impl_->automationOpenMenu == menu &&
                !ImGui::IsPopupOpen(
                    menu))
            {
                ImGui::OpenPopup(
                    menu);
            }

            if (!ImGui::BeginMenu(menu))
            {
                continue;
            }

            for (const MenuAction& action :
                 impl_->menuActions)
            {
                if (action.menu != menu)
                {
                    continue;
                }

                const bool enabled =
                    !action.enabled ||
                    action.enabled();

                if (ImGui::MenuItem(
                        action.label.c_str(),
                        action.shortcut.empty()
                            ? nullptr
                            : action.shortcut.c_str(),
                        false,
                        enabled))
                {
                    action.invoke();
                }
            }

            if (isView)
            {
                bool first = true;
                DockRegion lastRegion =
                    DockRegion::Auto;

                for (const PanelMenuEntry& entry :
                     panelMenu)
                {
                    if (!first &&
                        entry.region != lastRegion)
                    {
                        ImGui::Separator();
                    }

                    first = false;
                    lastRegion = entry.region;

                    if (ImGui::MenuItem(
                            entry.title.c_str(),
                            nullptr,
                            entry.open))
                    {
                        // Clicking an open panel that is hidden behind
                        // another tab brings it forward; clicking a visible
                        // one closes it; clicking a closed one opens it.
                        const ImGuiWindow* window =
                            ImGui::FindWindowByName(
                                entry.title.c_str());
                        const bool hiddenTab =
                            entry.open &&
                            window != nullptr &&
                            window->DockNode != nullptr &&
                            !window->DockTabIsVisible;

                        if (entry.open &&
                            !hiddenTab)
                        {
                            static_cast<void>(
                                SetPanelOpen(
                                    entry.panel,
                                    false));
                        }
                        else
                        {
                            static_cast<void>(
                                FocusPanel(
                                    entry.panel));
                        }
                    }
                }

                if (hasLayoutPanels)
                {
                    ImGui::Separator();

                    if (ImGui::MenuItem(
                            "Reset Layout"))
                    {
                        impl_->layoutBuildPending =
                            true;
                    }
                }
            }

            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }

    PanelContext context(
        impl_->automationExpandTrees,
        impl_->automationTraceWidgets
            ? &impl_->automationTrace
            : nullptr);

    for (std::size_t index = 0;
         index < impl_->panels.size();
         ++index)
    {
        if (impl_->panelOpen[index] == 0U)
        {
            continue;
        }

        PanelDefinition& panel =
            impl_->panels[index];

        bool open =
            impl_->panelOpen[index] != 0U;

        if (!impl_->pendingFocusTitle.empty() &&
            panel.title == impl_->pendingFocusTitle)
        {
            ImGui::SetNextWindowFocus();
            impl_->pendingFocusTitle.clear();
        }

        if (panel.minSize.width > 0.0F ||
            panel.minSize.height > 0.0F)
        {
            ImGui::SetNextWindowSizeConstraints(
                ImVec2(
                    panel.minSize.width,
                    panel.minSize.height),
                ImVec2(
                    FLT_MAX,
                    FLT_MAX));
        }

        if (panel.dockToMainViewport)
        {
            ImGui::SetNextWindowDockID(
                mainDockspace,
                ImGuiCond_Always);
        }

        if (ImGui::Begin(
                panel.title.c_str(),
                &open))
        {
            panel.draw(context);
        }

        ImGui::End();

        impl_->panelOpen[index] =
            open ? 1U : 0U;
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
            // List-local index: DrawIndexed applies the list's base vertex,
            // so adding it here as well would offset every list after the
            // first twice.
            impl_->convertedIndices.push_back(
                static_cast<u32>(index));
        }

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

    // Windows may report ImGui coordinates in DPI-virtualized logical
    // pixels while the swapchain is sized in physical pixels. Vertex
    // projection naturally spans the physical viewport, but clip rectangles
    // must be scaled explicitly or text and controls are truncated at 125%+
    // display scaling. Deriving the factor from the actual render target also
    // handles resize races more robustly than relying on backend-populated IO.
    const f32 framebufferScaleX =
        static_cast<f32>(targetWidth) /
        displayWidth;
    const f32 framebufferScaleY =
        static_cast<f32>(targetHeight) /
        displayHeight;

    const UiProjection projection =
        MakeUiProjection(
            drawData->DisplayPos.x,
            drawData->DisplayPos.y,
            displayWidth,
            displayHeight);

    const std::array<u32, 4>
        constants{
            std::bit_cast<u32>(
                projection.scaleX),
            std::bit_cast<u32>(
                projection.scaleY),
            std::bit_cast<u32>(
                projection.translateX),
            std::bit_cast<u32>(
                projection.translateY)
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
                    framebufferScaleX,
                (draw.ClipRect.y -
                 drawData->DisplayPos.y) *
                    framebufferScaleY
            };

            const ImVec2 clipMax{
                (draw.ClipRect.z -
                 drawData->DisplayPos.x) *
                    framebufferScaleX,
                (draw.ClipRect.w -
                 drawData->DisplayPos.y) *
                    framebufferScaleY
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

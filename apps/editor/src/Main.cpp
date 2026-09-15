#include <orbit/core/Log.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/editor_model/OutputLog.hpp>
#include <orbit/editor_ui/BodyPreviewRenderer.hpp>
#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/frames/FrameGraph.hpp>
#include <orbit/platform/Paths.hpp>
#include <orbit/render_graph/RenderGraph.hpp>
#include <orbit/render_view/RenderView.hpp>
#include <orbit/rhi/vulkan/VulkanBackend.hpp>
#include <orbit/runtime/RuntimeSession.hpp>
#include <orbit/shader/dxc/DxcShaderCompiler.hpp>
#include <orbit/universe/BodyRegistry.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <stdexcept>
#include <string>

namespace
{
[[nodiscard]] orbit::documents::ProjectDocument
OpenProject(
    const int argc,
    char** argv)
{
    std::filesystem::path manifest;

    if (argc >= 2 &&
        argv[1] != nullptr)
    {
        manifest =
            std::filesystem::path(
                argv[1]);

        if (std::filesystem::is_directory(
                manifest))
        {
            manifest /=
                "Project.orbit.toml";
        }
    }
    else
    {
        const std::filesystem::path local =
            std::filesystem::current_path() /
            "Project.orbit.toml";

        if (std::filesystem::exists(local))
        {
            manifest = local;
        }
    }

    if (!manifest.empty())
    {
        return orbit::documents::
            ProjectDocument::Open(
                manifest);
    }

    const std::filesystem::path scratch =
        orbit::platform::
            UserDataDirectory() /
        "Scratch" /
        "StudioPreview";

    const std::filesystem::path
        scratchManifest =
            scratch /
            "Project.orbit.toml";

    if (std::filesystem::exists(
            scratchManifest))
    {
        return orbit::documents::
            ProjectDocument::Open(
                scratchManifest);
    }

    return orbit::documents::
        ProjectDocument::Create(
            scratch,
            "Orbit Studio Preview");
}

[[nodiscard]] const char* LogPrefix(
    const orbit::log::Level level) noexcept
{
    switch (level)
    {
    case orbit::log::Level::Trace:
        return "TRACE";
    case orbit::log::Level::Info:
        return "INFO";
    case orbit::log::Level::Warning:
        return "WARN";
    case orbit::log::Level::Error:
        return "ERROR";
    }

    return "LOG";
}
} // namespace

int main(
    const int argc,
    char** argv)
{
    try
    {
        orbit::editor_model::OutputLog
            outputLog;

        orbit::documents::ProjectDocument
            project =
                OpenProject(
                    argc,
                    argv);

        const std::string windowTitle =
            std::format(
                "Orbit Studio - {}",
                project.Manifest().
                    displayName);

        orbit::runtime::RuntimeSession runtime({
            .applicationName = "OrbitStudio",
            .windowTitle = windowTitle,
            .width = 1680,
            .height = 980,
            .swapchainBufferCount = 3,
            .allowTearing = true,
            .relativeMouseMode = false
        });

        orbit::platform::Window& window =
            runtime.Window();
        orbit::rhi::Device& device =
            runtime.Device();
        orbit::rhi::Queue& graphicsQueue =
            runtime.GraphicsQueue();
        orbit::rhi::Swapchain& swapchain =
            runtime.Swapchain();

        const orbit::shader::dxc::
            DxcShaderCompiler compiler;

        orbit::frames::FrameGraph frames;
        orbit::universe::BodyRegistry bodies(
            frames);

        const orbit::universe::SystemId
            system =
                bodies.CreateSystem(
                    "Helion");

        const orbit::universe::BodyId
            bodyId =
                bodies.CreateBody({
                    .system = system,
                    .name = "Asterra",
                    .shape =
                        orbit::universe::
                            SphereShape{
                                .radiusMeters =
                                    6'000'000.0
                            },
                    .mass =
                        orbit::universe::
                            MassProperties{
                                .massKilograms =
                                    5.0e24
                            }
                });

        const orbit::universe::CelestialBody*
            activeBody =
                bodies.FindBody(bodyId);

        if (activeBody == nullptr)
        {
            throw std::runtime_error(
                "Studio failed to create its active body.");
        }

        orbit::render_view::RenderView
            bodyView(
                device,
                {
                    .width = 960,
                    .height = 640
                });

        orbit::editor_ui::
            BodyPreviewRenderer
                bodyPreview(
                    device,
                    compiler);

        const std::filesystem::path
            layoutPath =
                orbit::platform::
                    UserDataDirectory() /
                "EditorLayouts" /
                (project.Manifest().
                     projectId.ToString() +
                 ".ini");

        orbit::editor_ui::EditorUi ui(
            device,
            graphicsQueue,
            compiler,
            layoutPath);

        constexpr orbit::editor_ui::PanelId
            kViewportPanel{
                .high =
                    0x4f52424954535455ULL,
                .low =
                    0x44494f5657455750ULL
            };

        constexpr orbit::editor_ui::PanelId
            kOutputPanel{
                .high =
                    0x4f52424954535455ULL,
                .low =
                    0x44494f4f55545054ULL
            };

        ui.RegisterPanel({
            .id = kViewportPanel,
            .title = "Viewport",
            .defaultOpen = true,
            .draw =
                [&bodyView,
                 activeBody](
                    orbit::editor_ui::
                        PanelContext& context)
                {
                    const auto available =
                        context.ContentAvailable();

                    const orbit::u32 width =
                        static_cast<orbit::u32>(
                            std::max(
                                available.width,
                                1.0F));

                    const orbit::u32 height =
                        static_cast<orbit::u32>(
                            std::max(
                                available.height,
                                1.0F));

                    if (width !=
                            bodyView.Width() ||
                        height !=
                            bodyView.Height())
                    {
                        bodyView.Resize(
                            width,
                            height);
                    }

                    context.Image(
                        bodyView.Color(),
                        {
                            .width =
                                static_cast<
                                    orbit::f32>(
                                        bodyView.
                                            Width()),
                            .height =
                                static_cast<
                                    orbit::f32>(
                                        bodyView.
                                            Height())
                        });

                    context.Text(
                        activeBody->name);
                }
        });

        ui.RegisterPanel({
            .id = kOutputPanel,
            .title = "Output",
            .defaultOpen = true,
            .draw =
                [&outputLog](
                    orbit::editor_ui::
                        PanelContext& context)
                {
                    if (context.Button("Clear"))
                    {
                        outputLog.Clear();
                    }

                    context.Separator();

                    for (const auto& entry :
                         outputLog.Snapshot())
                    {
                        context.Text(
                            std::format(
                                "[{}] {}",
                                LogPrefix(
                                    entry.level),
                                entry.message));
                    }
                }
        });

        ui.RegisterMenuAction({
            .menu = "File",
            .label = "Save Project",
            .invoke =
                [&project]
                {
                    project.Save();
                    orbit::log::Info(
                        "Project manifest saved.");
                }
        });

        orbit::log::Info(
            std::format(
                "Opened project '{}' ({})",
                project.Manifest().
                    displayName,
                project.Manifest().
                    projectId.ToString()));

        auto allocator =
            device.CreateCommandAllocator(
                orbit::rhi::QueueType::
                    Graphics);

        auto commands =
            device.CreateCommandList(
                *allocator);

        auto fence =
            device.CreateFence(0);

        orbit::u64 submittedFence = 0;
        orbit::u64 nextFence = 1;

        using Clock =
            std::chrono::steady_clock;

        auto previous =
            Clock::now();

        while (window.PumpEvents())
        {
            if (submittedFence != 0)
            {
                fence->Wait(
                    submittedFence);
            }

            const auto now =
                Clock::now();

            const orbit::f64
                deltaSeconds =
                    std::clamp(
                        std::chrono::duration<
                            orbit::f64>(
                                now -
                                previous).
                            count(),
                        1.0 / 1000.0,
                        0.1);

            previous = now;

            const orbit::u32 width =
                window.Width();
            const orbit::u32 height =
                window.Height();

            if (width == 0 ||
                height == 0)
            {
                continue;
            }

            static_cast<void>(
                runtime.
                    ResizeSwapchainToWindow());

            ui.BeginFrame(
                window,
                deltaSeconds);

            ui.DrawStudioShell();

            if (window.KeyDown(
                    orbit::platform::
                        Key::Escape) &&
                !ui.WantsKeyboard())
            {
                break;
            }

            allocator->Reset();
            commands->Reset(
                *allocator);

            auto& backBuffer =
                swapchain.
                    CurrentBackBuffer();

            orbit::render_graph::
                RenderGraph graph(device);

            const auto viewTargets =
                bodyView.Import(
                    graph,
                    "StudioBody");

            const auto backBufferTarget =
                graph.ImportTexture(
                    "StudioSwapchain",
                    backBuffer,
                    orbit::rhi::
                        ResourceState::
                            Present);

            graph.AddPass(
                "Studio.BodyPreview",
                {
                    {
                        .texture =
                            viewTargets.color,
                        .state =
                            orbit::rhi::
                                ResourceState::
                                    RenderTarget,
                        .access =
                            orbit::render_graph::
                                Access::Write
                    }
                },
                [&](orbit::rhi::CommandList&
                        commandList,
                    const orbit::render_graph::
                        Resources&)
                {
                    bodyPreview.Draw(
                        commandList,
                        bodyView.Color(),
                        bodyView.Width(),
                        bodyView.Height(),
                        activeBody->shape);
                });

            graph.AddPass(
                "Studio.Canvas",
                {
                    {
                        .texture =
                            backBufferTarget,
                        .state =
                            orbit::rhi::
                                ResourceState::
                                    RenderTarget,
                        .access =
                            orbit::render_graph::
                                Access::Write
                    }
                },
                [&](orbit::rhi::CommandList&
                        commandList,
                    const orbit::render_graph::
                        Resources&)
                {
                    commandList.
                        ClearColorTarget(
                            backBuffer,
                            {
                                .red =
                                    0.018F,
                                .green =
                                    0.021F,
                                .blue =
                                    0.027F,
                                .alpha =
                                    1.0F
                            });

                    commandList.
                        SetRenderTarget(
                            backBuffer);
                });

            graph.AddPass(
                "Studio.Ui",
                {
                    {
                        .texture =
                            viewTargets.color,
                        .state =
                            orbit::rhi::
                                ResourceState::
                                    ShaderResource,
                        .access =
                            orbit::render_graph::
                                Access::Read
                    },
                    {
                        .texture =
                            backBufferTarget,
                        .state =
                            orbit::rhi::
                                ResourceState::
                                    RenderTarget,
                        .access =
                            orbit::render_graph::
                                Access::Write
                    }
                },
                [&](orbit::rhi::CommandList&
                        commandList,
                    const orbit::render_graph::
                        Resources&)
                {
                    ui.Render(
                        commandList,
                        backBuffer,
                        swapchain.Width(),
                        swapchain.Height());
                });

            graph.AddPass(
                "Studio.Present",
                {
                    {
                        .texture =
                            backBufferTarget,
                        .state =
                            orbit::rhi::
                                ResourceState::
                                    Present,
                        .access =
                            orbit::render_graph::
                                Access::Read
                    }
                },
                {});

            graph.Execute(*commands);

            commands->Close();
            graphicsQueue.Submit(
                *commands);

            swapchain.Present(true);

            submittedFence =
                nextFence++;

            graphicsQueue.Signal(
                *fence,
                submittedFence);
        }

        if (submittedFence != 0)
        {
            fence->Wait(
                submittedFence);
        }

        return 0;
    }
    catch (const std::exception& exception)
    {
        orbit::log::Error(
            exception.what());
        return 1;
    }
}

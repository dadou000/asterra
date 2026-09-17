#include <orbit/studio_ui/StudioViewportRenderer.hpp>

#include <optional>
#include <stdexcept>
#include <utility>

namespace orbit::studio_ui
{
StudioViewportRenderer::StudioViewportRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
    : bodyRenderer_(device, compiler),
      pathRenderer_(device, compiler)
{
}

std::vector<StudioRenderedView>
StudioViewportRenderer::Compose(
    render_graph::RenderGraph& graph,
    StudioRenderViewSet& views,
    studio_session::StudioSession& session,
    studio_session::StudioRuntimeBinding& runtime,
    const studio_session::StudioRuntimeSnapshot& snapshot,
    const time::SimulationTime atTime,
    const bool drawPathDebug)
{
    static_cast<void>(views.Refresh(snapshot));

    const universe::BodyRegistry* bodies = nullptr;
    const frames::FrameGraph* frames = nullptr;
    std::vector<const path_geometry::PathDerivedProduct*> products;

    if (snapshot.hasWorld)
    {
        bodies = &runtime.Bodies(snapshot);
        frames = &runtime.Frames(snapshot);
        products = runtime.PathProducts(snapshot).Products();
    }

    std::vector<StudioRenderedView> rendered;
    const auto catalog = views.Catalog();
    rendered.reserve(catalog.size());

    for (const auto& info : catalog)
    {
        auto* view = views.Find(info.id);

        if (view == nullptr)
        {
            throw std::logic_error(
                "Studio RenderView catalog contains a missing view.");
        }

        const std::string prefix =
            "StudioViewport." + info.id;
        const auto targets =
            view->Import(
                graph,
                prefix.c_str());

        const auto* logicalTarget =
            session.Viewports().Find(info.id);

        if (logicalTarget == nullptr)
        {
            throw std::logic_error(
                "Studio RenderView has no logical viewport target.");
        }

        std::optional<universe::BodyShape> shape;

        if (logicalTarget->target.has_value())
        {
            if (!snapshot.hasWorld || bodies == nullptr ||
                logicalTarget->target->universeGeneration !=
                    snapshot.universeGeneration)
            {
                throw std::logic_error(
                    "Studio viewport render target is stale for the current universe generation.");
            }

            const auto* body =
                bodies->FindBody(
                    logicalTarget->target->body);

            if (body == nullptr)
            {
                throw std::logic_error(
                    "Studio viewport target body is missing from the current BodyRegistry.");
            }

            shape = body->shape;
        }

        if (shape.has_value())
        {
            const auto camera = view->Camera();
            const auto bodyShape = *shape;
            const u32 width = view->Width();
            const u32 height = view->Height();
            auto* color = &view->Color();

            graph.AddPass(
                prefix + ".Body",
                {
                    {
                        .texture = targets.color,
                        .state = rhi::ResourceState::RenderTarget,
                        .access = render_graph::Access::Write
                    }
                },
                [this,
                 color,
                 width,
                 height,
                 camera,
                 bodyShape](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    bodyRenderer_.Draw(
                        commands,
                        *color,
                        width,
                        height,
                        bodyShape,
                        camera);
                });

            if (frames != nullptr &&
                !products.empty())
            {
                const auto* frameGraph = frames;
                const auto pathProducts = products;

                graph.AddPass(
                    prefix + ".Paths",
                    {
                        {
                            .texture = targets.color,
                            .state = rhi::ResourceState::RenderTarget,
                            .access = render_graph::Access::Write
                        }
                    },
                    [this,
                     color,
                     width,
                     height,
                     camera,
                     frameGraph,
                     pathProducts,
                     atTime,
                     drawPathDebug](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        pathRenderer_.Draw(
                            commands,
                            *color,
                            width,
                            height,
                            camera,
                            *frameGraph,
                            atTime,
                            std::span<
                                const path_geometry::PathDerivedProduct* const>(
                                    pathProducts.data(),
                                    pathProducts.size()),
                            drawPathDebug);
                    });
            }
        }
        else
        {
            auto* color = &view->Color();

            graph.AddPass(
                prefix + ".Blank",
                {
                    {
                        .texture = targets.color,
                        .state = rhi::ResourceState::RenderTarget,
                        .access = render_graph::Access::Write
                    }
                },
                [color](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    commands.ClearColorTarget(
                        *color,
                        {
                            .red = 0.018F,
                            .green = 0.021F,
                            .blue = 0.027F,
                            .alpha = 1.0F
                        });
                });
        }

        rendered.push_back({
            .id = info.id,
            .targets = targets,
            .targeted = shape.has_value()
        });
    }

    return rendered;
}
} // namespace orbit::studio_ui

#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/studio_session/BezierHandleEditor.hpp>
#include <orbit/studio_session/WorldBoundPathNetwork.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace
{
void Check(const bool condition)
{
    if (!condition)
    {
        std::cerr << "Bezier handle editor test failed.\n";
        std::exit(1);
    }
}

bool Equal(
    const orbit::math::Double3& a,
    const orbit::math::Double3& b)
{
    return a.x == b.x &&
        a.y == b.y &&
        a.z == b.z;
}
} // namespace

int main()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-bezier-handle-editor-" +
         orbit::documents::ProjectId::Random().ToString());
    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "Bezier Handle Editor Test");
        orbit::editor_session::EditorWorldSession world(project);
        orbit::studio_session::WorldBoundPathNetwork boundPaths(world);
        Check(boundPaths.RefreshBinding());
        Check(boundPaths.HasService());

        auto& paths = boundPaths.Service();
        const auto network =
            paths.CreateNetwork("Roads");
        const auto frame =
            orbit::frames::FrameId::Random();
        const auto start =
            paths.CreateNode(
                network.id,
                "Start",
                orbit::paths::FramePointAnchor{
                    .frame = frame,
                    .localMeters = {0.0, 0.0, 0.0}
                });
        const auto end =
            paths.CreateNode(
                network.id,
                "End",
                orbit::paths::FramePointAnchor{
                    .frame = frame,
                    .localMeters = {100.0, 0.0, 0.0}
                });

        const orbit::math::Double3 originalStart{
            20.0, 0.0, 0.0};
        const orbit::math::Double3 originalEnd{
            -20.0, 0.0, 0.0};
        const auto edge =
            paths.ConnectBezier(
                start.id,
                end.id,
                originalStart,
                originalEnd,
                "Curve");

        orbit::studio_session::BezierHandleEditor editor(
            world,
            boundPaths);

        editor.Begin(
            edge.id,
            orbit::studio_session::BezierHandle::Start);
        Check(editor.Active());

        const orbit::math::Double3 intermediate{
            25.0, 4.0, 0.0};
        const orbit::math::Double3 finalStart{
            30.0, 8.0, 2.0};
        editor.Update(intermediate);
        editor.Update(finalStart);
        editor.Commit();
        Check(!editor.Active());

        auto edited = paths.FindEdge(edge.id);
        Check(edited.has_value());
        Check(Equal(edited->startHandleMeters, finalStart));
        Check(Equal(edited->endHandleMeters, originalEnd));

        // A complete viewport drag is one undo step despite multiple updates.
        Check(world.Commands().CanUndo());
        world.Commands().Undo();
        auto undone = paths.FindEdge(edge.id);
        Check(undone.has_value());
        Check(Equal(undone->startHandleMeters, originalStart));
        Check(Equal(undone->endHandleMeters, originalEnd));

        Check(world.Commands().CanRedo());
        world.Commands().Redo();
        auto redone = paths.FindEdge(edge.id);
        Check(redone.has_value());
        Check(Equal(redone->startHandleMeters, finalStart));
        Check(Equal(redone->endHandleMeters, originalEnd));

        editor.Begin(
            edge.id,
            orbit::studio_session::BezierHandle::End);
        editor.Update({-35.0, 9.0, 1.0});
        editor.Cancel();
        auto cancelled = paths.FindEdge(edge.id);
        Check(cancelled.has_value());
        Check(Equal(cancelled->startHandleMeters, finalStart));
        Check(Equal(cancelled->endHandleMeters, originalEnd));
        Check(!world.Commands().HasActiveTransaction());

        const auto secondary =
            project.CreateWorld("Secondary", "Secondary");
        const u64 firstBinding = boundPaths.BindingGeneration();
        world.OpenWorld(secondary.relativePath);
        Check(boundPaths.RefreshBinding());
        Check(boundPaths.BindingGeneration() > firstBinding);
        Check(boundPaths.HasService());
        Check(!boundPaths.Service().FindEdge(edge.id).has_value());

        world.OpenWorld("Main");
        Check(boundPaths.RefreshBinding());
        const auto reopened =
            boundPaths.Service().FindEdge(edge.id);
        Check(reopened.has_value());
        Check(Equal(reopened->startHandleMeters, finalStart));
    }

    std::filesystem::remove_all(root);
    return 0;
}

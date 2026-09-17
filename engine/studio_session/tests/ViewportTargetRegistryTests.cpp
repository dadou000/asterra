#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/editor_session/ActiveBodyModel.hpp>
#include <orbit/editor_session/EditorWorldSession.hpp>
#include <orbit/studio_session/ViewportTargetRegistry.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace
{
void Check(const bool condition)
{
    if (!condition)
    {
        std::cerr << "Viewport target registry test failed.\n";
        std::exit(1);
    }
}

orbit::scene::ObjectId AddBody(
    orbit::editor_session::EditorWorldSession& session,
    const orbit::scene::ObjectId parent,
    const std::string_view name,
    const double radius)
{
    const auto body =
        session.Commands().CreateObject(
            orbit::world_model::kCelestialBodyType,
            name,
            parent);
    session.Commands().SetProperty(
        body,
        orbit::world_model::kBodyRadius,
        radius);
    session.Commands().SetProperty(
        body,
        orbit::world_model::kBodyMass,
        5.0e24);
    return body;
}
} // namespace

int main()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-viewport-targets-" +
         orbit::documents::ProjectId::Random().ToString());
    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "Viewport Target Test");
        orbit::editor_session::EditorWorldSession world(project);

        const auto worldRoot =
            world.Commands().CreateObject(
                orbit::world_model::kWorldType,
                "World");
        const auto system =
            world.Commands().CreateObject(
                orbit::world_model::kCelestialSystemType,
                "Helion",
                worldRoot);
        const auto asterra =
            AddBody(
                world,
                system,
                "Asterra",
                6'000'000.0);
        const auto luma =
            AddBody(
                world,
                asterra,
                "Luma",
                1'700'000.0);

        orbit::editor_session::ActiveBodyModel active(world);
        static_cast<void>(active.Refresh());

        orbit::studio_session::ViewportTargetRegistry views(
            world,
            active);
        views.Register(
            "perspective",
            orbit::studio_session::ViewportMode::Perspective,
            true);
        views.Register(
            "map",
            orbit::studio_session::ViewportMode::BodyMap,
            false);
        views.PinToObject("map", luma);

        const auto* perspective = views.Find("perspective");
        const auto* map = views.Find("map");
        Check(perspective != nullptr && perspective->target.has_value());
        Check(map != nullptr && map->target.has_value());
        Check(perspective->target->semanticObject == asterra);
        Check(map->target->semanticObject == luma);
        Check(perspective->target->body != map->target->body);

        const std::array selection{luma};
        world.Selection().Set(selection);
        Check(views.Refresh());
        Check(
            views.Find("perspective")->target->semanticObject ==
            luma);
        Check(
            views.Find("map")->target->semanticObject ==
            luma);

        views.PinToObject("perspective", asterra);
        world.Selection().Clear();
        static_cast<void>(views.Refresh());
        Check(
            views.Find("perspective")->target->semanticObject ==
            asterra);
        Check(!views.Find("perspective")->followActiveBody);

        const auto secondary =
            project.CreateWorld("Secondary", "Secondary");
        world.OpenWorld(secondary);
        Check(views.Refresh());
        Check(!views.Find("perspective")->target.has_value());
        Check(!views.Find("map")->target.has_value());
        Check(
            !views.Find("perspective")->
                pinnedSemanticObject.has_value());
        Check(
            !views.Find("map")->
                pinnedSemanticObject.has_value());

        const auto secondaryRoot =
            world.Commands().CreateObject(
                orbit::world_model::kWorldType,
                "Secondary World");
        const auto secondarySystem =
            world.Commands().CreateObject(
                orbit::world_model::kCelestialSystemType,
                "Veyra System",
                secondaryRoot);
        const auto veyra =
            AddBody(
                world,
                secondarySystem,
                "Veyra",
                4'200'000.0);

        views.FollowActiveBody("perspective");
        Check(views.Refresh());
        Check(
            views.Find("perspective")->target->semanticObject ==
            veyra);
        Check(!views.Find("map")->target.has_value());

        views.FollowActiveBody("map");
        static_cast<void>(views.Refresh());
        Check(
            views.Find("map")->target->semanticObject ==
            veyra);
        Check(
            views.Find("map")->mode ==
            orbit::studio_session::ViewportMode::BodyMap);

        Check(views.Catalog().size() == 2U);
        Check(views.Unregister("map"));
        Check(views.Find("map") == nullptr);
    }

    std::filesystem::remove_all(root);
    return 0;
}

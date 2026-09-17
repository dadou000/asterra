#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/editor_session/ActiveBodyModel.hpp>
#include <orbit/editor_session/EditorWorldSession.hpp>
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
        std::cerr << "Active body model test failed.\n";
        std::exit(1);
    }
}

orbit::scene::ObjectId PopulateWorld(
    orbit::editor_session::EditorWorldSession& session,
    const std::string_view bodyName,
    const double radius)
{
    auto& commands = session.Commands();

    const auto world =
        commands.CreateObject(
            orbit::world_model::kWorldType,
            "World");
    const auto system =
        commands.CreateObject(
            orbit::world_model::kCelestialSystemType,
            "Helion",
            world);
    const auto body =
        commands.CreateObject(
            orbit::world_model::kCelestialBodyType,
            bodyName,
            system);

    commands.SetProperty(
        body,
        orbit::world_model::kBodyRadius,
        radius);
    commands.SetProperty(
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
        ("orbit-active-body-model-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "Active Body Model Test");
        orbit::editor_session::EditorWorldSession session(project);

        const auto asterra =
            PopulateWorld(
                session,
                "Asterra",
                6'000'000.0);

        const auto asterraRecord =
            session.Objects().Find(asterra);
        Check(asterraRecord.has_value());
        Check(asterraRecord->parent.has_value());

        const auto luma =
            session.Commands().CreateObject(
                orbit::world_model::kCelestialBodyType,
                "Luma",
                asterra);
        session.Commands().SetProperty(
            luma,
            orbit::world_model::kBodyRadius,
            1'700'000.0);
        session.Commands().SetProperty(
            luma,
            orbit::world_model::kBodyMass,
            7.0e22);

        orbit::editor_session::ActiveBodyModel active(session);

        Check(active.Refresh());
        Check(active.Active().has_value());
        Check(active.Active()->semanticObject == asterra);
        Check(active.Active()->name == "Asterra");
        Check(
            active.Active()->referenceRadiusMeters ==
            6'000'000.0);
        Check(
            active.Active()->universeGeneration ==
            session.UniverseGeneration());

        const std::array moonSelection{luma};
        session.Selection().Set(moonSelection);

        Check(active.Refresh());
        Check(active.Active()->semanticObject == luma);
        const auto stableMoonBody = active.Active()->body;
        const auto moonUniverseGeneration =
            active.Active()->universeGeneration;
        Check(active.Active()->name == "Luma");

        session.Commands().SetProperty(
            luma,
            orbit::world_model::kBodyRadius,
            1'800'000.0);

        Check(active.Refresh());
        Check(active.Active()->body == stableMoonBody);
        Check(
            active.Active()->referenceRadiusMeters ==
            1'800'000.0);
        Check(
            active.Active()->universeGeneration >
            moonUniverseGeneration);
        Check(
            active.Active()->universeGeneration ==
            session.UniverseGeneration());

        session.Selection().Clear();
        Check(!active.Refresh());
        Check(active.Active()->semanticObject == luma);

        active.Focus(asterra);
        Check(active.Active()->semanticObject == asterra);
        Check(
            active.Active()->universeGeneration ==
            session.UniverseGeneration());

        const auto firstGeneration =
            session.Generation();
        const auto secondary =
            project.CreateWorld(
                "Secondary",
                "Secondary");
        session.OpenWorld(secondary);

        Check(session.Generation() > firstGeneration);
        Check(active.Refresh());
        Check(!active.Active().has_value());

        const auto veyra =
            PopulateWorld(
                session,
                "Veyra",
                4'200'000.0);

        Check(active.Refresh());
        Check(active.Active().has_value());
        Check(active.Active()->semanticObject == veyra);
        Check(active.Active()->name == "Veyra");
        Check(
            active.Active()->sessionGeneration ==
            session.Generation());
        Check(
            active.Active()->universeGeneration ==
            session.UniverseGeneration());

        session.CloseWorld();
        Check(active.Refresh());
        Check(!active.Active().has_value());
    }

    std::filesystem::remove_all(root);
    return 0;
}

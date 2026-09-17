#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/studio_session/UniverseBoundPathCache.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace
{
void Check(const bool condition)
{
    if (!condition)
    {
        std::cerr << "Universe-bound path cache test failed.\n";
        std::exit(1);
    }
}

orbit::scene::ObjectId AddBody(
    orbit::editor_session::EditorWorldSession& world)
{
    const auto root =
        world.Commands().CreateObject(
            orbit::world_model::kWorldType,
            "World");
    const auto system =
        world.Commands().CreateObject(
            orbit::world_model::kCelestialSystemType,
            "Helion",
            root);
    const auto body =
        world.Commands().CreateObject(
            orbit::world_model::kCelestialBodyType,
            "Asterra",
            system);
    world.Commands().SetProperty(
        body,
        orbit::world_model::kBodyRadius,
        6'000'000.0);
    return body;
}
} // namespace

int main()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-universe-path-cache-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "Universe Path Cache Test");
        orbit::editor_session::EditorWorldSession world(project);

        const auto bodyObject = AddBody(world);
        Check(world.RefreshUniverseIfChanged());

        const auto bodyId =
            world.Universe().BodyForObject(bodyObject);
        Check(bodyId.has_value());
        const auto* body =
            world.Universe().Bodies().FindBody(*bodyId);
        Check(body != nullptr);

        orbit::studio_session::UniverseBoundPathCache cache(world);
        Check(cache.Empty());
        Check(cache.BindingGeneration() == 1U);

        const auto edge = orbit::scene::ObjectId::Random();
        orbit::path_geometry::PathDerivedProduct product{
            .edge = edge,
            .frame = body->frame,
            .sourceRevision = world.Objects().Revision(),
            .buildSignature = 1234U,
            .widthMeters = 8.0,
            .laneCount = 2U
        };

        cache.Store(product, 7U);
        Check(cache.Size() == 1U);
        Check(cache.Find(edge) != nullptr);
        Check(cache.Find(edge)->buildSignature == 1234U);
        Check(cache.RouteGeneration(edge).value_or(0U) == 7U);
        Check(cache.Products().size() == 1U);

        bool foreignFrameRejected = false;
        try
        {
            auto invalid = product;
            invalid.edge = orbit::scene::ObjectId::Random();
            invalid.frame = orbit::frames::FrameId::Random();
            cache.Store(std::move(invalid));
        }
        catch (const std::invalid_argument&)
        {
            foreignFrameRejected = true;
        }
        Check(foreignFrameRejected);
        Check(cache.Size() == 1U);

        const auto oldBindingGeneration =
            cache.BindingGeneration();
        static_cast<void>(world.RebuildUniverse());

        // Stale products are never exposed, even before the owner explicitly
        // refreshes the cache binding.
        Check(cache.Empty());
        Check(cache.Find(edge) == nullptr);
        Check(!cache.RouteGeneration(edge).has_value());
        Check(cache.RefreshBinding());
        Check(cache.Empty());
        Check(
            cache.BindingGeneration() >
            oldBindingGeneration);

        const auto rebuiltBodyId =
            world.Universe().BodyForObject(bodyObject);
        Check(rebuiltBodyId.has_value());
        const auto* rebuiltBody =
            world.Universe().Bodies().FindBody(*rebuiltBodyId);
        Check(rebuiltBody != nullptr);

        product.frame = rebuiltBody->frame;
        product.buildSignature = 5678U;
        cache.Store(product);
        Check(cache.Size() == 1U);
        Check(!cache.RouteGeneration(edge).has_value());

        world.CloseWorld();
        Check(cache.Empty());
        Check(cache.RefreshBinding());
        Check(cache.Empty());

        world.OpenWorld("Main");
        Check(cache.RefreshBinding());
        Check(cache.Empty());
    }

    std::filesystem::remove_all(root);
    return 0;
}

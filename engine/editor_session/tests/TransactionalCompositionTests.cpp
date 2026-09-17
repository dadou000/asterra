#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/editor_session/EditorWorldSession.hpp>
#include <orbit/universe/ReferenceSurface.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <filesystem>

namespace
{
[[nodiscard]] double RuntimeRadius(
    orbit::editor_session::EditorWorldSession& session,
    const orbit::scene::ObjectId bodyObject)
{
    const auto bodyId = session.Universe().BodyForObject(bodyObject);
    if (!bodyId.has_value())
    {
        return -1.0;
    }

    const auto* body = session.Universe().Bodies().FindBody(*bodyId);
    if (body == nullptr)
    {
        return -1.0;
    }

    return orbit::universe::ReferenceRadiusMeters(body->shape);
}
} // namespace

int main()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-transactional-composition-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project = orbit::documents::ProjectDocument::Create(
            root,
            "Transactional Composition Test");
        orbit::editor_session::EditorWorldSession session(project);

        const auto worldObject = session.Commands().CreateObject(
            orbit::world_model::kWorldType,
            "World");
        const auto systemObject = session.Commands().CreateObject(
            orbit::world_model::kCelestialSystemType,
            "Helion",
            worldObject);
        const auto bodyObject = session.Commands().CreateObject(
            orbit::world_model::kCelestialBodyType,
            "Asterra",
            systemObject);
        session.Commands().SetProperty(
            bodyObject,
            orbit::world_model::kBodyRadius,
            6'000'000.0);

        if (!session.RefreshUniverseIfChanged() ||
            RuntimeRadius(session, bodyObject) != 6'000'000.0)
        {
            return 1;
        }

        const orbit::u64 committedRevision = session.Objects().Revision();
        const orbit::u64 composedRevision = session.Universe().SourceRevision();

        if (committedRevision != composedRevision)
        {
            return 2;
        }

        session.Commands().BeginTransaction("Uncommitted radius");
        session.Commands().SetProperty(
            bodyObject,
            orbit::world_model::kBodyRadius,
            6'100'000.0);

        if (session.Objects().Revision() != committedRevision)
        {
            return 3;
        }

        if (session.RefreshUniverseIfChanged())
        {
            return 4;
        }

        if (RuntimeRadius(session, bodyObject) != 6'000'000.0)
        {
            return 5;
        }

        session.Commands().RollbackTransaction();

        if (session.Objects().Revision() != committedRevision ||
            session.RefreshUniverseIfChanged() ||
            RuntimeRadius(session, bodyObject) != 6'000'000.0)
        {
            return 6;
        }

        session.Commands().BeginTransaction("Committed radius");
        session.Commands().SetProperty(
            bodyObject,
            orbit::world_model::kBodyRadius,
            6'200'000.0);
        session.Commands().CommitTransaction();

        if (session.Objects().Revision() == committedRevision)
        {
            return 7;
        }

        if (!session.RefreshUniverseIfChanged())
        {
            return 8;
        }

        if (RuntimeRadius(session, bodyObject) != 6'200'000.0 ||
            session.Universe().SourceRevision() != session.Objects().Revision())
        {
            return 9;
        }

        session.Checkpoint();
    }

    std::filesystem::remove_all(root);
    return 0;
}

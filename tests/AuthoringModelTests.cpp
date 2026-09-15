#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/selection/SelectionService.hpp>

#include <array>
#include <cassert>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>

int main()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-authoring-" +
         orbit::documents::ProjectId::Random().
             ToString());

    std::filesystem::remove_all(root);

    const orbit::schema::TypeId folderType{
        .high = 0x1000,
        .low = 0x0001
    };

    const orbit::schema::TypeId nodeType{
        .high = 0x1000,
        .low = 0x0002
    };

    const orbit::schema::PropertyId speedProperty{
        .high = 0x2000,
        .low = 0x0001
    };

    orbit::schema::SchemaRegistry schemas;

    schemas.RegisterType({
        .id = folderType,
        .displayName = "Folder",
        .category = "Organization"
    });

    schemas.RegisterType({
        .id = nodeType,
        .displayName = "Path Node",
        .category = "Path",
        .properties = {
            orbit::schema::PropertySchema{
                .id = speedProperty,
                .name = "Target Speed",
                .kind =
                    orbit::schema::
                        PropertyKind::Float,
                .unit = "m/s",
                .defaultValue = 10.0,
                .range = {
                    .minimum = 0.0,
                    .maximum = 100.0
                }
            }
        }
    });

    orbit::scene::ObjectId rootObject{};
    orbit::scene::ObjectId childObject{};

    {
        auto project =
            orbit::documents::
                ProjectDocument::Create(
                    root,
                    "Authoring Test");

        orbit::documents::WorldDatabase world(
            project.StartupWorldPath());

        assert(
            world.SchemaVersion() ==
            orbit::documents::
                kCurrentWorldSchemaVersion);

        orbit::scene::ObjectStore objects(
            world);

        orbit::commands::CommandService commands(
            objects,
            schemas);

        orbit::selection::SelectionService
            selection;

        rootObject =
            commands.CreateObject(
                folderType,
                "Network");

        childObject =
            commands.CreateObject(
                nodeType,
                "Node A",
                rootObject);

        const auto roots =
            objects.Roots();
        const auto children =
            objects.Children(
                rootObject);

        assert(roots.size() == 1);
        assert(roots.front().id == rootObject);
        assert(children.size() == 1);
        assert(
            children.front().id ==
            childObject);

        // Invalid schema value is rejected before touching persistence.
        bool invalidRejected = false;

        try
        {
            commands.SetProperty(
                childObject,
                speedProperty,
                120.0);
        }
        catch (const std::invalid_argument&)
        {
            invalidRejected = true;
        }

        assert(invalidRejected);
        assert(
            !objects.GetProperty(
                childObject,
                speedProperty).
                has_value());

        // A rolled-back editor transaction must leave the database
        // byte-for-byte semantically unchanged to readers.
        commands.BeginTransaction(
            "Discarded edit");

        commands.RenameObject(
            childObject,
            "Temporary Name");

        commands.SetProperty(
            childObject,
            speedProperty,
            35.0);

        commands.RollbackTransaction();

        assert(
            objects.Find(childObject)->
                name ==
            "Node A");

        assert(
            !objects.GetProperty(
                childObject,
                speedProperty).
                has_value());

        // One semantic edit can contain multiple commands while
        // remaining one undo unit and one SQLite transaction.
        commands.BeginTransaction(
            "Configure node");

        commands.RenameObject(
            childObject,
            "Node Alpha");

        commands.SetProperty(
            childObject,
            speedProperty,
            42.0);

        commands.ReparentObject(
            childObject,
            std::nullopt);

        commands.CommitTransaction();

        assert(
            objects.Find(childObject)->
                name ==
            "Node Alpha");

        assert(
            objects.Find(childObject)->
                parent ==
            std::nullopt);

        assert(
            std::get<orbit::f64>(
                *objects.GetProperty(
                    childObject,
                    speedProperty)) ==
            42.0);

        assert(commands.CanUndo());
        commands.Undo();

        assert(
            objects.Find(childObject)->
                name ==
            "Node A");

        assert(
            objects.Find(childObject)->
                parent ==
            std::optional<
                orbit::scene::ObjectId>(
                    rootObject));

        assert(
            !objects.GetProperty(
                childObject,
                speedProperty).
                has_value());

        assert(commands.CanRedo());
        commands.Redo();

        assert(
            objects.Find(childObject)->
                name ==
            "Node Alpha");

        assert(
            std::get<orbit::f64>(
                *objects.GetProperty(
                    childObject,
                    speedProperty)) ==
            42.0);

        const std::array<
            orbit::scene::ObjectId,
            3> requestedSelection{
                childObject,
                rootObject,
                childObject
            };

        selection.Set(
            std::span(
                requestedSelection));

        assert(
            selection.Ordered().size() ==
            2);
        assert(
            selection.Ordered()[0] ==
            childObject);
        assert(
            selection.Ordered()[1] ==
            rootObject);

        const orbit::u64
            selectionRevision =
                selection.Revision();

        selection.Toggle(rootObject);

        assert(
            selection.Ordered().size() ==
            1);
        assert(
            selection.Revision() ==
            selectionRevision + 1);

        world.Checkpoint();
    }

    // Reopen the authoritative world through a new connection. The
    // committed command transaction must persist; undo history itself
    // is session state and intentionally is not persisted here.
    {
        const auto project =
            orbit::documents::
                ProjectDocument::Open(
                    root /
                    "Project.orbit.toml");

        orbit::documents::WorldDatabase world(
            project.StartupWorldPath());

        orbit::scene::ObjectStore objects(
            world);

        const auto rootRecord =
            objects.Find(rootObject);

        const auto childRecord =
            objects.Find(childObject);

        assert(rootRecord.has_value());
        assert(childRecord.has_value());
        assert(
            childRecord->name ==
            "Node Alpha");
        assert(
            childRecord->parent ==
            std::nullopt);

        const auto speed =
            objects.GetProperty(
                childObject,
                speedProperty);

        assert(speed.has_value());
        assert(
            std::get<orbit::f64>(*speed) ==
            42.0);
    }

    std::filesystem::remove_all(root);
    return 0;
}

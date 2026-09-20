#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <thread>

namespace
{
void Check(const bool condition)
{
    if (!condition)
    {
        std::cerr
            << "Studio M12 physical page service test failed.\n";
        std::exit(1);
    }
}

[[nodiscard]] orbit::scene::ObjectId
OnlyChildOfType(
    orbit::studio_session::StudioSession& studio,
    const orbit::scene::ObjectId parent,
    const orbit::schema::TypeId type)
{
    orbit::scene::ObjectId result{};
    orbit::u32 count = 0U;

    for (const auto& child :
         studio.World().Objects().
             Children(parent))
    {
        if (child.type != type)
        {
            continue;
        }

        result = child.id;
        ++count;
    }

    Check(count == 1U);
    return result;
}

void DriveReady(
    orbit::studio_session::StudioSession& studio,
    const orbit::terrain::PhysicalTerrainPageAddress& address,
    const orbit::u64 differentFrom = 0U)
{
    for (orbit::u32 iteration = 0U;
         iteration < 20'000U;
         ++iteration)
    {
        static_cast<void>(
            studio.Tick(false));

        const auto status =
            studio.TerrainPhysicalPages().
                PageStatus(address);

        const auto page =
            studio.TerrainPhysicalPages().
                Find(address);

        if (status.has_value() &&
            status->state ==
                orbit::studio_session::
                    TerrainRebuildState::Ready &&
            page != nullptr &&
            page->revisionFingerprint !=
                differentFrom)
        {
            return;
        }

        std::this_thread::yield();
    }

    Check(false);
}
} // namespace

int main()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-studio-m12-pages-" +
         orbit::documents::ProjectId::Random().
             ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "Studio M12 Physical Pages");

        orbit::studio_session::StudioSession
            studio(project);

        studio.Viewports().Register(
            "studio.primary",
            orbit::studio_session::
                ViewportMode::Perspective,
            true);

        const auto worldObject =
            studio.World().Commands().
                CreateObject(
                    orbit::world_model::kWorldType,
                    "World");

        const orbit::scene::ObjectId selected[]{
            worldObject};

        studio.World().Selection().Set(
            selected);

        studio.World().CommandRegistry().Invoke(
            orbit::editor_model::
                authoring_commands::
                    kCreateRockyPlanet,
            {
                {
                    "name",
                    std::string("Asterra")
                }
            });

        Check(
            studio.World().Selection().
                Ordered().size() == 1U);

        const auto body =
            studio.World().Selection().
                Ordered().front();

        const auto terrain =
            OnlyChildOfType(
                studio,
                body,
                orbit::world_model::
                    kTerrainSurfaceType);

        const auto process =
            OnlyChildOfType(
                studio,
                terrain,
                orbit::world_model::
                    kTerrainProcessAssetType);

        auto& commands =
            studio.World().Commands();

        commands.SetProperty(
            process,
            orbit::world_model::
                kProcessStreamPowerEnabled,
            false);
        commands.SetProperty(
            process,
            orbit::world_model::
                kProcessHydraulicEnabled,
            false);
        commands.SetProperty(
            process,
            orbit::world_model::
                kProcessThermalEnabled,
            false);
        commands.SetProperty(
            process,
            orbit::world_model::
                kProcessAeolianEnabled,
            false);
        commands.SetProperty(
            process,
            orbit::world_model::
                kProcessGlacialEnabled,
            false);
        commands.SetProperty(
            process,
            orbit::world_model::
                kProcessRiversEnabled,
            false);
        commands.SetProperty(
            process,
            orbit::world_model::
                kProcessCoastalEnabled,
            false);

        static_cast<void>(
            studio.Tick(false));

        const auto runtime =
            studio.TerrainRuntime().
                Capture(
                    "studio.primary");

        Check(runtime.has_value());

        const auto address =
            runtime->
                observerPhysicalPage;

        DriveReady(
            studio,
            address);

        const auto first =
            studio.TerrainPhysicalPages().
                Find(address);

        Check(first != nullptr);
        Check(first->material != nullptr);
        Check(first->debugPage != nullptr);
        Check(
            first->debugPage->
                Stamp().address ==
            address);
        Check(
            first->debugPage->
                Stamp().revisions ==
            first->revisions);

        const orbit::u64
            firstFingerprint =
                first->
                    revisionFingerprint;

        const auto firstRevisions =
            first->revisions;

        commands.SetProperty(
            terrain,
            orbit::world_model::
                kTerrainMacroAmplitudeMeters,
            1'450.0);

        studio.QueueTerrainInvalidation({
            .kind =
                orbit::terrain_dependency::
                    TerrainChangeKind::
                        TerrainAuthoring,
            .scope = {
                .planet =
                    address.planet,
                .global = true
            }
        });

        // First Studio tick composes the new semantic source and hands the
        // invalidation to M06 without waiting for its debounce window.
        static_cast<void>(
            studio.Tick(false));

        studio.TerrainPhysicalPages().
            RebuildDirty();

        Check(
            studio.TerrainDebugPages().
                Find(address) ==
            nullptr);

        Check(
            studio.TerrainPhysicalPages().
                Find(address) ==
            nullptr);

        DriveReady(
            studio,
            address,
            firstFingerprint);

        const auto second =
            studio.TerrainPhysicalPages().
                Find(address);

        Check(second != nullptr);
        Check(
            second->
                revisionFingerprint !=
            firstFingerprint);
        Check(
            second->revisions.authoring >
            firstRevisions.authoring);
        Check(
            second->debugPage !=
            first->debugPage);
        Check(
            studio.TerrainDebugPages().
                Find(address) ==
            second->debugPage);

        const auto status =
            studio.TerrainPhysicalPages().
                PageStatus(address);

        Check(status.has_value());
        Check(
            status->state ==
            orbit::studio_session::
                TerrainRebuildState::Ready);
        Check(
            status->dirtyProducts == 0U);
    }

    std::filesystem::remove_all(root);
    return 0;
}

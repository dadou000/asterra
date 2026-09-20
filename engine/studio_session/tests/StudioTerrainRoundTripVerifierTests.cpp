#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/editor_model/SurfaceAuthoringModel.hpp>
#include <orbit/studio_session/StudioTerrainRoundTripVerifier.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace
{
void Check(const bool condition)
{
    if (!condition)
    {
        std::cerr
            << "Studio terrain round-trip verifier test failed.\n";
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
} // namespace

int main()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-studio-m14-round-trip-" +
         orbit::documents::ProjectId::Random().
             ToString());

    std::filesystem::remove_all(root);

    {
        orbit::studio_session::StudioWorkspace
            workspace;

        workspace.CreateProject(
            root,
            "M14 Round Trip");

        auto& studio =
            workspace.Session();

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

        const orbit::scene::ObjectId
            selected[]{
                worldObject
            };

        studio.World().Selection().Set(
            selected);

        studio.World().
            CommandRegistry().
            Invoke(
                orbit::editor_model::
                    authoring_commands::
                        kCreateRockyPlanet,
                {
                    {
                        "name",
                        std::string(
                            "Asterra")
                    }
                });

        Check(
            studio.World().Selection().
                Ordered().size() ==
            1U);

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

        orbit::editor_model::
            SurfaceAuthoringModel
            surface(
                studio.World().Objects(),
                studio.World().Commands(),
                studio.World().Selection());

        const std::vector<
            orbit::math::Double3>
            canyonPoints{
                {0.0, 0.0, -1.0},
                {0.01, 0.0, -0.99995},
                {0.02, 0.0, -0.9998}
            };

        const auto canyon =
            surface.AddCanyonSpline(
                terrain,
                canyonPoints,
                300.0,
                500.0,
                80.0);

        Check(canyon.IsValid());

        static_cast<void>(
            studio.Tick(false));

        const auto report =
            orbit::studio_session::
                VerifyStudioTerrainRoundTrip(
                    workspace,
                    "studio.primary");

        if (!report.success)
        {
            std::cerr
                << "M14 failure stage: "
                << report.failureStage
                << " | "
                << report.diagnostic
                << '\n';
        }

        Check(report.success);
        Check(
            report.semanticBody ==
            body);
        Check(
            report.terrainObject ==
            terrain);
        Check(
            report.semanticIdsPreserved);
        Check(
            report.derivedCacheFreshAfterReopen);
        Check(
            report.debugResidencyFreshAfterReopen);
        Check(
            report.comparisonPagePreserved);
        Check(
            report.semanticFingerprintBefore !=
                0U &&
            report.semanticFingerprintBefore ==
                report.semanticFingerprintAfter);
        Check(
            report.physicalFingerprintBefore !=
                0U &&
            report.physicalFingerprintBefore ==
                report.physicalFingerprintAfter);

        Check(workspace.HasProject());
        Check(
            workspace.Session().
                World().
                Objects().
                Find(body).
                has_value());
        Check(
            workspace.Session().
                World().
                Objects().
                Find(canyon).
                has_value());
    }

    std::filesystem::remove_all(root);
    return 0;
}

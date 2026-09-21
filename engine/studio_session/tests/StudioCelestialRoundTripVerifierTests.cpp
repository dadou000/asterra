#include <orbit/studio_session/StudioCelestialRoundTripVerifier.hpp>

#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/PropertyProvenanceSchema.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <filesystem>
#include <iostream>
#include <string>

int main()
{
    using namespace orbit;
    using namespace orbit::world_model;

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-celestial-roundtrip-" +
         documents::ProjectId::Random().
             ToString());

    std::filesystem::remove_all(root);

    studio_session::StudioWorkspace workspace;
    workspace.CreateProject(
        root,
        "Celestial Round Trip");

    auto& world =
        workspace.Session().
            World();

    auto& commands =
        world.Commands();

    scene::ObjectId worldRoot{};

    for (const auto& rootObject :
         world.Objects().Roots())
    {
        if (rootObject.type ==
            kWorldType)
        {
            worldRoot =
                rootObject.id;
            break;
        }
    }

    if (!worldRoot)
        return 1;

    const auto system =
        commands.CreateObject(
            kCelestialSystemType,
            "Test System",
            worldRoot);

    const auto body =
        commands.CreateObject(
            kCelestialBodyType,
            "Test Giant",
            system);

    const auto giant =
        commands.CreateObject(
            kGiantAppearanceCapabilityType,
            "Giant Appearance",
            body);

    commands.SetProperty(
        giant,
        kCapabilityModel,
        std::string{
            "Procedural Giant"});

    const auto provenance =
        commands.CreateObject(
            kPropertyProvenanceType,
            "Giant Provenance",
            giant);

    commands.SetProperty(
        provenance,
        kProvenanceTargetProperty,
        std::string{
            "GiantAppearance"});
    commands.SetProperty(
        provenance,
        kProvenanceSourceMode,
        i64{static_cast<i64>(
            PropertySourceMode::
                Procedural)});
    commands.SetProperty(
        provenance,
        kProvenanceSolveState,
        i64{static_cast<i64>(
            PropertySolveState::
                Solved)});

    world.Checkpoint();

    const auto report =
        studio_session::
            VerifyStudioCelestialRoundTrip(
                workspace,
                body);

    if (!report.success)
    {
        std::cerr
            << "round-trip failure stage="
            << report.failureStage
            << " diagnostic="
            << report.diagnostic
            << '\n';
        return 2;
    }

    if (!report.semanticIdsPreserved ||
        !report.provenancePreserved ||
        !report.freshWorkspaceRecomposition ||
        !report.derivedProductsRegenerated)
    {
        return 3;
    }

    if (report.semanticFingerprintBefore !=
            report.semanticFingerprintAfter ||
        report.runtimeOrbitFingerprintBefore !=
            report.runtimeOrbitFingerprintAfter ||
        report.
                derivedAppearanceFingerprintBefore !=
            report.
                derivedAppearanceFingerprintAfter ||
        report.
                representationFingerprintBefore !=
            report.
                representationFingerprintAfter)
    {
        return 4;
    }

    std::filesystem::remove_all(root);
    return 0;
}

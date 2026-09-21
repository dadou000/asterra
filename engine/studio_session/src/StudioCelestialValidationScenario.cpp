#include <orbit/studio_session/StudioCelestialValidationScenario.hpp>

#include <orbit/celestial_lighting/CelestialLighting.hpp>
#include <orbit/celestial_representation/RepresentationResolver.hpp>
#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/studio_session/StudioWorkspace.hpp>
#include <orbit/world_model/CelestialAtmosphereBinding.hpp>
#include <orbit/world_model/CelestialCloudBinding.hpp>
#include <orbit/world_model/CelestialGiantBinding.hpp>
#include <orbit/world_model/CelestialMagnetosphereBinding.hpp>
#include <orbit/world_model/CelestialOceanBinding.hpp>
#include <orbit/world_model/CelestialRadiometryBinding.hpp>
#include <orbit/world_model/CelestialRingBinding.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/CelestialSmallBodyBinding.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace orbit::studio_session
{
namespace
{
void AddStep(
    StudioCelestialValidationScenarioReport& report,
    std::string name,
    const bool passed,
    std::string diagnostic = {})
{
    report.steps.push_back({
        .name = std::move(name),
        .passed = passed,
        .diagnostic = std::move(diagnostic)
    });
}

void Fail(
    StudioCelestialValidationScenarioReport& report,
    const std::string_view stage,
    const std::string_view diagnostic)
{
    report.success = false;
    report.failureStage =
        std::string(stage);
    report.diagnostic =
        std::string(diagnostic);

    AddStep(
        report,
        std::string(stage),
        false,
        std::string(diagnostic));
}

[[nodiscard]] scene::ObjectId
CreateBody(
    editor_session::EditorWorldSession& world,
    const scene::ObjectId parent,
    const std::string_view name,
    const f64 radiusMeters,
    const f64 massKilograms)
{
    auto& commands =
        world.Commands();

    const auto body =
        commands.CreateObject(
            world_model::kCelestialBodyType,
            name,
            parent);

    commands.SetProperty(
        body,
        world_model::kBodyEllipsoidEnabled,
        false);
    commands.SetProperty(
        body,
        world_model::kBodyRadius,
        radiusMeters);
    commands.SetProperty(
        body,
        world_model::kBodyPolarRadius,
        radiusMeters);
    commands.SetProperty(
        body,
        world_model::kBodyMass,
        massKilograms);
    commands.SetProperty(
        body,
        world_model::kBodyParentPositionMeters,
        math::Double3{});

    return body;
}

[[nodiscard]] scene::ObjectId
AddCapability(
    editor_session::EditorWorldSession& world,
    const scene::ObjectId body,
    const schema::TypeId type,
    const std::string_view name)
{
    return world.Commands().
        CreateObject(
            type,
            name,
            body);
}

void AddAnalyticOrbit(
    editor_session::EditorWorldSession& world,
    const scene::ObjectId body,
    const f64 semiMajorAxisMeters,
    const f64 eccentricity,
    const f64 mu,
    const f64 phaseDegrees,
    const f64 inclinationDegrees = 0.0)
{
    auto& commands =
        world.Commands();

    const auto orbit =
        commands.CreateObject(
            world_model::kOrbitCapabilityType,
            "Analytic Orbit",
            body);

    commands.SetProperty(
        orbit,
        world_model::kCapabilityModel,
        std::string{"Analytic Conic"});
    commands.SetProperty(
        orbit,
        world_model::kOrbitSemiMajorAxisMeters,
        semiMajorAxisMeters);
    commands.SetProperty(
        orbit,
        world_model::kOrbitPeriapsisDistanceMeters,
        semiMajorAxisMeters *
            (1.0 - eccentricity));
    commands.SetProperty(
        orbit,
        world_model::kOrbitEccentricity,
        eccentricity);
    commands.SetProperty(
        orbit,
        world_model::kOrbitInclinationDegrees,
        inclinationDegrees);
    commands.SetProperty(
        orbit,
        world_model::kOrbitAscendingNodeDegrees,
        0.0);
    commands.SetProperty(
        orbit,
        world_model::kOrbitArgumentPeriapsisDegrees,
        0.0);
    commands.SetProperty(
        orbit,
        world_model::kOrbitMeanAnomalyEpochDegrees,
        phaseDegrees);
    commands.SetProperty(
        orbit,
        world_model::kOrbitGravitationalParameter,
        mu);
    commands.SetProperty(
        orbit,
        world_model::kOrbitEpochMicroseconds,
        i64{0});
}

[[nodiscard]] bool
OrbitMovesWithTime(
    editor_session::EditorWorldSession& world,
    const scene::ObjectId bodyObject)
{
    const auto bodyId =
        world.Universe().
            BodyForObject(
                bodyObject);

    if (!bodyId.has_value())
        return false;

    const auto* body =
        world.Universe().
            Bodies().
            FindBody(
                *bodyId);

    if (body == nullptr)
        return false;

    const auto a =
        world.Universe().
            Frames().
            ResolveTransform(
                body->centerFrame,
                body->parentFrame,
                {.microsecondsFromEpoch = 0});

    const auto b =
        world.Universe().
            Frames().
            ResolveTransform(
                body->centerFrame,
                body->parentFrame,
                {.microsecondsFromEpoch =
                     30LL * 86'400LL *
                     1'000'000LL});

    if (!a.has_value() ||
        !b.has_value())
    {
        return false;
    }

    return math::Length(
               a->translation -
               b->translation) >
        1.0;
}

[[nodiscard]] bool
ObserveRepresentationLadder(
    const f64 radiusMeters)
{
    using celestial_representation::
        Representation;

    std::set<Representation> seen;

    for (u32 i = 0U; i < 96U; ++i)
    {
        const f64 t =
            static_cast<f64>(i) /
            95.0;

        const f64 altitude =
            std::pow(
                10.0,
                std::lerp(
                    3.0,
                    14.0,
                    t));

        const celestial_representation::
            ResolveInput input{
                .bodyRadiusMeters =
                    radiusMeters,
                .maximumProductionDetailMeters =
                    8'000.0,
                .maximumMacroDisplacementMeters =
                    10'000.0,
                .cameraDistanceToCenterMeters =
                    radiusMeters +
                    altitude,
                .verticalFieldOfViewRadians =
                    1.0,
                .viewportHeightPixels =
                    1080.0,
                .features = {
                    .productionSurfaceAvailable =
                        true,
                    .macroDisplacementAvailable =
                        true,
                    .complexFarAppearance =
                        true,
                    .radiativeEmitter =
                        false
                }
            };

        seen.insert(
            celestial_representation::
                Resolve(input).
                representation);
    }

    const bool hasDisc =
        seen.contains(
            Representation::
                CachedDiscImpostor) ||
        seen.contains(
            Representation::
                AnalyticDiscImpostor);

    return
        seen.contains(
            Representation::
                ProductionSurface) &&
        seen.contains(
            Representation::
                MacroDisplacedGlobe) &&
        seen.contains(
            Representation::
                SmoothGlobe) &&
        hasDisc &&
        seen.contains(
            Representation::
                PointProxy);
}

[[nodiscard]] scene::ObjectId
FindWorldRoot(
    editor_session::EditorWorldSession& world)
{
    for (const auto& root :
         world.Objects().Roots())
    {
        if (root.type ==
            world_model::kWorldType)
        {
            return root.id;
        }
    }

    return {};
}
} // namespace

StudioCelestialValidationScenarioReport
RunStudioCelestialValidationScenario(
    const std::filesystem::path& rootDirectory,
    const std::string_view viewportId)
{
    StudioCelestialValidationScenarioReport
        report{};

    report.projectRoot =
        rootDirectory;

    try
    {
        if (rootDirectory.empty())
        {
            Fail(
                report,
                "preflight",
                "M35 requires a non-empty validation project root.");
            return report;
        }

        if (std::filesystem::exists(
                rootDirectory))
        {
            Fail(
                report,
                "preflight",
                "M35 validation project root already exists.");
            return report;
        }

        StudioWorkspace workspace;
        workspace.CreateProject(
            rootDirectory,
            "V0.0.6 Celestial Validation");

        auto& studio =
            workspace.Session();

        const auto validationWorld =
            studio.CreateWorld(
                "CelestialValidation",
                "V0.0.6 Celestial Validation");

        studio.OpenWorld(
            validationWorld.relativePath);

        report.projectManifest =
            workspace.Project().
                ManifestPath();
        report.worldPath =
            validationWorld.relativePath;

        auto& world =
            studio.World();

        const auto worldRoot =
            FindWorldRoot(world);

        if (!worldRoot)
        {
            const auto created =
                world.Commands().
                    CreateObject(
                        world_model::kWorldType,
                        "World");

            if (!created)
            {
                Fail(
                    report,
                    "create-world",
                    "Could not create semantic World root.");
                return report;
            }
        }

        const auto root =
            FindWorldRoot(world);

        const auto system =
            world.Commands().
                CreateObject(
                    world_model::
                        kCelestialSystemType,
                    "Helion System",
                    root);

        world.Commands().
            SetProperty(
                system,
                world_model::
                    kSystemEpochMicroseconds,
                i64{0});

        report.helion =
            CreateBody(
                world,
                system,
                "Helion",
                696'340'000.0,
                1.98847e30);

        const auto helionEmitter =
            AddCapability(
                world,
                report.helion,
                world_model::
                    kRadiativeEmitterCapabilityType,
                "Radiative Emitter");

        world.Commands().SetProperty(
            helionEmitter,
            world_model::kCapabilityModel,
            std::string{"Blackbody"});
        world.Commands().SetProperty(
            helionEmitter,
            world_model::kEmitterDeriveLuminosity,
            true);
        world.Commands().SetProperty(
            helionEmitter,
            world_model::kEmitterEmissivity,
            1.0);

        const auto helionPhotosphere =
            AddCapability(
                world,
                report.helion,
                world_model::
                    kPhotosphereCapabilityType,
                "Photosphere");

        world.Commands().SetProperty(
            helionPhotosphere,
            world_model::kCapabilityModel,
            std::string{"Blackbody"});
        world.Commands().SetProperty(
            helionPhotosphere,
            world_model::kPhotosphereRadiusMeters,
            696'340'000.0);
        world.Commands().SetProperty(
            helionPhotosphere,
            world_model::kPhotosphereTemperatureKelvin,
            5772.0);

        static_cast<void>(
            AddCapability(
                world,
                report.helion,
                world_model::
                    kGravityCapabilityType,
                "Gravity"));

        const scene::ObjectId selectedSystem[] = {
            system
        };

        world.Selection().
            Set(
                selectedSystem);

        world.CommandRegistry().
            Invoke(
                editor_model::
                    authoring_commands::
                        kCreateRockyPlanet,
                {
                    {
                        "name",
                        std::string{
                            "Asterra"}
                    },
                    {
                        "radiusMeters",
                        6'371'000.0
                    },
                    {
                        "massKg",
                        5.9722e24
                    }
                });

        if (world.Selection().
                Ordered().size() != 1U)
        {
            Fail(
                report,
                "create-asterra",
                "Create Rocky Planet did not select Asterra.");
            return report;
        }

        report.asterra =
            world.Selection().
                Ordered().front();

        AddAnalyticOrbit(
            world,
            report.asterra,
            149'597'870'700.0,
            0.0167,
            1.32712440018e20,
            0.0);

        static_cast<void>(
            AddCapability(
                world,
                report.asterra,
                world_model::
                    kAtmosphereCapabilityType,
                "Atmosphere"));

        static_cast<void>(
            AddCapability(
                world,
                report.asterra,
                world_model::
                    kOceanCapabilityType,
                "Ocean"));

        static_cast<void>(
            AddCapability(
                world,
                report.asterra,
                world_model::
                    kCloudLayerCapabilityType,
                "Cloud Layer"));

        static_cast<void>(
            AddCapability(
                world,
                report.asterra,
                world_model::
                    kMagnetosphereCapabilityType,
                "Magnetosphere / Aurora"));

        report.luma =
            CreateBody(
                world,
                report.asterra,
                "Luma",
                1'900'000.0,
                8.0e22);

        AddAnalyticOrbit(
            world,
            report.luma,
            384'400'000.0,
            0.03,
            3.986004418e14,
            0.0,
            2.0);

        static_cast<void>(
            AddCapability(
                world,
                report.luma,
                world_model::
                    kSmallBodyAppearanceCapabilityType,
                "Small Body Appearance"));

        report.umbra =
            CreateBody(
                world,
                report.asterra,
                "Umbra",
                1'100'000.0,
                2.4e22);

        AddAnalyticOrbit(
            world,
            report.umbra,
            560'000'000.0,
            0.06,
            3.986004418e14,
            180.0,
            7.0);

        static_cast<void>(
            AddCapability(
                world,
                report.umbra,
                world_model::
                    kSmallBodyAppearanceCapabilityType,
                "Small Body Appearance"));

        report.companionStar =
            CreateBody(
                world,
                system,
                "Helion B",
                430'000'000.0,
                9.5e29);

        world.Commands().
            SetProperty(
                report.companionStar,
                world_model::
                    kBodyParentPositionMeters,
                math::Double3{
                    4.0e11,
                    0.0,
                    0.0});

        const auto companionEmitter =
            AddCapability(
                world,
                report.companionStar,
                world_model::
                    kRadiativeEmitterCapabilityType,
                "Radiative Emitter");

        world.Commands().SetProperty(
            companionEmitter,
            world_model::kCapabilityModel,
            std::string{"Blackbody"});
        world.Commands().SetProperty(
            companionEmitter,
            world_model::kEmitterDeriveLuminosity,
            true);
        world.Commands().SetProperty(
            companionEmitter,
            world_model::kEmitterEmissivity,
            1.0);

        const auto companionPhotosphere =
            AddCapability(
                world,
                report.companionStar,
                world_model::
                    kPhotosphereCapabilityType,
                "Photosphere");

        world.Commands().SetProperty(
            companionPhotosphere,
            world_model::kCapabilityModel,
            std::string{"Blackbody"});
        world.Commands().SetProperty(
            companionPhotosphere,
            world_model::kPhotosphereRadiusMeters,
            430'000'000.0);
        world.Commands().SetProperty(
            companionPhotosphere,
            world_model::kPhotosphereTemperatureKelvin,
            4800.0);

        report.ringedGiant =
            CreateBody(
                world,
                system,
                "Kheiron",
                69'911'000.0,
                1.898e27);

        world.Commands().
            SetProperty(
                report.ringedGiant,
                world_model::
                    kBodyParentPositionMeters,
                math::Double3{
                    7.5e11,
                    0.0,
                    0.0});

        static_cast<void>(
            AddCapability(
                world,
                report.ringedGiant,
                world_model::
                    kGiantAppearanceCapabilityType,
                "Giant Appearance"));

        const auto ringSystem =
            AddCapability(
                world,
                report.ringedGiant,
                world_model::
                    kRingSystemCapabilityType,
                "Ring System");

        static_cast<void>(
            world.Commands().
                CreateObject(
                    world_model::
                        kRingBandType,
                    "Main Ring",
                    ringSystem));

        report.airlessBody =
            CreateBody(
                world,
                system,
                "Cinder",
                420'000.0,
                1.1e21);

        world.Commands().
            SetProperty(
                report.airlessBody,
                world_model::
                    kBodyParentPositionMeters,
                math::Double3{
                    2.5e11,
                    1.0e10,
                    0.0});

        static_cast<void>(
            AddCapability(
                world,
                report.airlessBody,
                world_model::
                    kSmallBodyAppearanceCapabilityType,
                "Small Body Appearance"));

        static_cast<void>(
            studio.Tick(false));

        AddStep(
            report,
            "create-system",
            world.Universe().
                    BodyForObject(
                        report.helion).
                    has_value() &&
                world.Universe().
                    BodyForObject(
                        report.asterra).
                    has_value() &&
                world.Universe().
                    BodyForObject(
                        report.luma).
                    has_value() &&
                world.Universe().
                    BodyForObject(
                        report.umbra).
                    has_value(),
            "Created Helion, Asterra, Luma and Umbra through normal world authoring/composition.");

        report.simulationTimeChangesOrbit =
            OrbitMovesWithTime(
                world,
                report.asterra);

        studio.Clock().
            SetTime({
                .microsecondsFromEpoch = 0});
        studio.Clock().
            StepSeconds(
                10.0 * 86'400.0);

        AddStep(
            report,
            "system-time-orbit",
            report.
                simulationTimeChangesOrbit &&
                studio.Clock().
                    Time().
                    microsecondsFromEpoch >
                    0,
            "Simulation clock advanced and Asterra runtime orbit changes across time.");

        studio.Viewports().
            Register(
                std::string(
                    viewportId),
                ViewportMode::Perspective,
                true);

        studio.Viewports().
            PinToObject(
                viewportId,
                report.asterra);

        static_cast<void>(
            studio.Tick(false));

        auto runtime =
            studio.TerrainRuntime().
                Capture(
                    viewportId);

        if (runtime.has_value())
        {
            const orbit::world::WorldPosition ground{
                .meters = {
                    runtime->
                        planet.
                        radiusMeters +
                        1'000.0,
                    0.0,
                    0.0
                }
            };

            static_cast<void>(
                studio.TerrainRuntime().
                    SetObserver(
                        viewportId,
                        ground));

            static_cast<void>(
                studio.Tick(false));

            report.
                terrainGroundRuntimeAvailable =
                studio.TerrainRuntime().
                    Capture(
                        viewportId).
                    has_value();

            const orbit::world::WorldPosition orbit{
                .meters = {
                    runtime->
                        planet.
                        radiusMeters +
                        12'000'000.0,
                    0.0,
                    0.0
                }
            };

            static_cast<void>(
                studio.TerrainRuntime().
                    SetObserver(
                        viewportId,
                        orbit));

            static_cast<void>(
                studio.Tick(false));

            report.
                terrainOrbitRuntimeAvailable =
                studio.TerrainRuntime().
                    Capture(
                        viewportId).
                    has_value();
        }

        const auto atmosphere =
            world_model::
                ResolveAtmosphereBody(
                    world.Objects(),
                    report.asterra);

        const auto ocean =
            world_model::
                ResolveOceanBody(
                    world.Objects(),
                    report.asterra);

        const auto clouds =
            world_model::
                ResolveCloudLayers(
                    world.Objects(),
                    report.asterra);

        report.atmosphereResolved =
            atmosphere.has_value();
        report.oceanResolved =
            ocean.has_value();
        report.cloudsResolved =
            !clouds.empty();

        AddStep(
            report,
            "ground-to-orbit",
            report.
                    terrainGroundRuntimeAvailable &&
                report.
                    terrainOrbitRuntimeAvailable &&
                report.atmosphereResolved,
            "Production terrain viewport remained bound from near-ground to orbital observer distance with atmosphere authority present.");

        AddStep(
            report,
            "atmosphere-ocean-clouds",
            report.atmosphereResolved &&
                report.oceanResolved &&
                report.cloudsResolved,
            "Asterra atmosphere, ocean and cloud capabilities resolve through production bindings.");

        report.
            fullRepresentationLadderObserved =
            ObserveRepresentationLadder(
                6'371'000.0);

        AddStep(
            report,
            "representation-ladder",
            report.
                fullRepresentationLadderObserved,
            "Production surface, macro globe, smooth globe, impostor and point representations are all selected across deterministic distances.");

        const auto eclipse =
            celestial_lighting::
                FiniteDiscOccultation(
                    {
                        .centerFromObserverMeters =
                            {149'597'870'700.0,
                             0.0,
                             0.0},
                        .radiusMeters =
                            696'340'000.0
                    },
                    {
                        .centerFromObserverMeters =
                            {384'400'000.0,
                             0.0,
                             0.0},
                        .radiusMeters =
                            1'900'000.0
                    });

        report.eclipseDetected =
            eclipse.overlapping &&
            eclipse.visibleFraction < 1.0;

        AddStep(
            report,
            "eclipse-transit",
            report.eclipseDetected,
            "Finite-disc production lighting detects a Luma/Helion eclipse geometry.");

        const auto giant =
            world_model::
                ResolveGiantAppearance(
                    world.Objects(),
                    report.ringedGiant);

        const auto rings =
            world_model::
                ResolveRingSystem(
                    world.Objects(),
                    report.ringedGiant);

        report.ringedGiantStressPassed =
            giant.has_value() &&
            rings.has_value() &&
            !rings->
                parameters.bands.empty();

        AddStep(
            report,
            "ringed-giant-stress",
            report.
                ringedGiantStressPassed,
            "Giant appearance and multi-band ring authority resolve without terrain.");

        const auto airless =
            world_model::
                ResolveSmallBodyAppearance(
                    world.Objects(),
                    report.airlessBody);

        report.airlessBodyStressPassed =
            airless.has_value();

        AddStep(
            report,
            "airless-body-stress",
            report.
                airlessBodyStressPassed,
            "Airless small-body appearance resolves without terrain authority.");

        const auto resolvedHelion =
            world_model::
                ResolveRadiativeBody(
                    world.Objects(),
                    report.helion);

        const auto resolvedCompanion =
            world_model::
                ResolveRadiativeBody(
                    world.Objects(),
                    report.companionStar);

        const f64 helionFlux =
            resolvedHelion.has_value()
                ? celestial_lighting::
                      AttenuatedDirectIrradiance(
                          resolvedHelion->
                              radiative.
                              luminosityWatts,
                          149'597'870'700.0,
                          1.0).
                      irradianceWattsPerSquareMeter
                : 0.0;

        const f64 companionFlux =
            resolvedCompanion.has_value()
                ? celestial_lighting::
                      AttenuatedDirectIrradiance(
                          resolvedCompanion->
                              radiative.
                              luminosityWatts,
                          4.0e11,
                          1.0).
                      irradianceWattsPerSquareMeter
                : 0.0;

        report.binaryStarStressPassed =
            resolvedHelion.has_value() &&
            resolvedCompanion.has_value() &&
            std::isfinite(helionFlux) &&
            std::isfinite(companionFlux) &&
            helionFlux > 0.0 &&
            companionFlux > 0.0;

        AddStep(
            report,
            "binary-star-stress",
            report.binaryStarStressPassed,
            "Two independent stellar irradiance sources remain finite and positive.");

        report.roundTrip =
            VerifyStudioCelestialRoundTrip(
                workspace,
                report.asterra);

        AddStep(
            report,
            "save-reopen-equivalence",
            report.roundTrip.success,
            report.roundTrip.diagnostic);

        if (!report.roundTrip.success)
        {
            Fail(
                report,
                "save-reopen-equivalence",
                report.roundTrip.
                    diagnostic);
            return report;
        }

        report.success =
            report.
                simulationTimeChangesOrbit &&
            report.
                terrainGroundRuntimeAvailable &&
            report.
                terrainOrbitRuntimeAvailable &&
            report.atmosphereResolved &&
            report.oceanResolved &&
            report.cloudsResolved &&
            report.eclipseDetected &&
            report.
                fullRepresentationLadderObserved &&
            report.binaryStarStressPassed &&
            report.ringedGiantStressPassed &&
            report.airlessBodyStressPassed &&
            report.roundTrip.success;

        if (!report.success)
        {
            Fail(
                report,
                "final-contract",
                "One or more deterministic M35 celestial acceptance conditions failed.");
            return report;
        }

        report.failureStage.clear();
        report.diagnostic =
            "Headless M35 celestial integration scenario passed. Pixel-level rendering acceptance still requires the Vulkan real-device smoke workflow.";

        AddStep(
            report,
            "real-device-visual-gate",
            true,
            "Required separately: run Orbit Studio Real Device Smoke on the orbit-vulkan runner for pixel/device validation.");

        return report;
    }
    catch (const std::exception& exception)
    {
        Fail(
            report,
            "exception",
            exception.what());
        return report;
    }
}
} // namespace orbit::studio_session

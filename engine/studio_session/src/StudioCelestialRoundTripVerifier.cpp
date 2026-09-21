#include <orbit/studio_session/StudioCelestialRoundTripVerifier.hpp>

#include <orbit/celestial_appearance/PlanetaryAppearance.hpp>
#include <orbit/celestial_compact_objects/CompactObject.hpp>
#include <orbit/celestial_giants/GiantAppearance.hpp>
#include <orbit/celestial_representation/RepresentationResolver.hpp>
#include <orbit/celestial_small_bodies/SmallBodyAppearance.hpp>
#include <orbit/terrain/TerrainContracts.hpp>
#include <orbit/world_model/CelestialCompactObjectBinding.hpp>
#include <orbit/world_model/CelestialGiantBinding.hpp>
#include <orbit/world_model/CelestialMagnetosphereBinding.hpp>
#include <orbit/world_model/CelestialSmallBodyBinding.hpp>
#include <orbit/world_model/PropertyProvenanceSchema.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace orbit::studio_session
{
namespace
{
[[nodiscard]] u64 HashText(
    u64 hash,
    const std::string_view text) noexcept
{
    hash =
        terrain::StableCombine64(
            hash,
            static_cast<u64>(
                text.size()));

    for (const unsigned char ch :
         text)
    {
        hash =
            terrain::StableCombine64(
                hash,
                static_cast<u64>(ch));
    }

    return hash;
}

[[nodiscard]] u64 HashDouble(
    const u64 hash,
    const f64 value) noexcept
{
    return terrain::StableCombine64(
        hash,
        std::bit_cast<u64>(value));
}

[[nodiscard]] u64 HashProperty(
    u64 hash,
    const schema::PropertyValue& value)
{
    hash =
        terrain::StableCombine64(
            hash,
            static_cast<u64>(
                value.index()));

    std::visit(
        [&](const auto& typed)
        {
            using T =
                std::decay_t<
                    decltype(typed)>;

            if constexpr (
                std::is_same_v<T, bool>)
            {
                hash =
                    terrain::StableCombine64(
                        hash,
                        typed ? 1U : 0U);
            }
            else if constexpr (
                std::is_same_v<T, i64>)
            {
                hash =
                    terrain::StableCombine64(
                        hash,
                        std::bit_cast<u64>(
                            typed));
            }
            else if constexpr (
                std::is_same_v<T, f64>)
            {
                hash =
                    HashDouble(
                        hash,
                        typed);
            }
            else if constexpr (
                std::is_same_v<T, std::string>)
            {
                hash =
                    HashText(
                        hash,
                        typed);
            }
            else if constexpr (
                std::is_same_v<T, math::Double3>)
            {
                hash =
                    HashDouble(
                        hash,
                        typed.x);
                hash =
                    HashDouble(
                        hash,
                        typed.y);
                hash =
                    HashDouble(
                        hash,
                        typed.z);
            }
            else if constexpr (
                std::is_same_v<
                    T,
                    schema::ObjectReferenceValue>)
            {
                hash =
                    terrain::StableCombine64(
                        hash,
                        typed.high);
                hash =
                    terrain::StableCombine64(
                        hash,
                        typed.low);
            }
        },
        value);

    return hash;
}

[[nodiscard]] std::vector<scene::ObjectRecord>
CollectSubtree(
    const scene::ObjectStore& objects,
    const scene::ObjectId root)
{
    const auto rootRecord =
        objects.Find(root);

    if (!rootRecord.has_value())
        return {};

    std::vector<scene::ObjectRecord> result;
    std::vector<scene::ObjectRecord> pending{
        *rootRecord};

    while (!pending.empty())
    {
        auto object =
            std::move(
                pending.back());
        pending.pop_back();

        auto children =
            objects.Children(
                object.id);

        pending.insert(
            pending.end(),
            children.begin(),
            children.end());

        result.push_back(
            std::move(object));
    }

    std::sort(
        result.begin(),
        result.end(),
        [](const scene::ObjectRecord& a,
           const scene::ObjectRecord& b)
        {
            return a.id < b.id;
        });

    return result;
}

struct SemanticFingerprint
{
    u64 full{0U};
    u64 provenance{0U};
    std::vector<scene::ObjectId> ids;
};

[[nodiscard]] SemanticFingerprint
FingerprintSemanticSubtree(
    const editor_session::EditorWorldSession& world,
    const scene::ObjectId body)
{
    SemanticFingerprint result{
        .full = 0x4d333343454c534dULL,
        .provenance = 0x4d333350524f5601ULL
    };

    const auto objects =
        CollectSubtree(
            world.Objects(),
            body);

    for (const auto& object :
         objects)
    {
        result.ids.push_back(
            object.id);

        u64 objectHash =
            terrain::StableCombine64(
                object.id.high,
                object.id.low);

        objectHash =
            terrain::StableCombine64(
                objectHash,
                object.type.high);
        objectHash =
            terrain::StableCombine64(
                objectHash,
                object.type.low);
        objectHash =
            HashText(
                objectHash,
                object.name);

        if (object.parent.has_value())
        {
            objectHash =
                terrain::StableCombine64(
                    objectHash,
                    object.parent->high);
            objectHash =
                terrain::StableCombine64(
                    objectHash,
                    object.parent->low);
        }

        const auto* type =
            world.Schemas().
                FindType(
                    object.type);

        if (type != nullptr)
        {
            auto properties =
                type->properties;

            std::sort(
                properties.begin(),
                properties.end(),
                [](const auto& a,
                   const auto& b)
                {
                    return a.id < b.id;
                });

            for (const auto& property :
                 properties)
            {
                objectHash =
                    terrain::StableCombine64(
                        objectHash,
                        property.id.high);
                objectHash =
                    terrain::StableCombine64(
                        objectHash,
                        property.id.low);

                const auto authored =
                    world.Objects().
                        GetProperty(
                            object.id,
                            property.id);

                objectHash =
                    HashProperty(
                        objectHash,
                        authored.has_value()
                            ? *authored
                            : property.defaultValue);
            }
        }

        result.full =
            terrain::StableCombine64(
                result.full,
                objectHash);

        if (object.type ==
            world_model::
                kPropertyProvenanceType)
        {
            result.provenance =
                terrain::StableCombine64(
                    result.provenance,
                    objectHash);
        }
    }

    return result;
}

[[nodiscard]] u64 HashTransform(
    u64 hash,
    const math::RigidTransformD& transform)
{
    hash =
        HashDouble(
            hash,
            transform.translation.x);
    hash =
        HashDouble(
            hash,
            transform.translation.y);
    hash =
        HashDouble(
            hash,
            transform.translation.z);
    const std::array axes{
        transform.rotation.xAxis,
        transform.rotation.yAxis,
        transform.rotation.zAxis
    };

    for (const auto& axis : axes)
    {
        hash =
            HashDouble(
                hash,
                axis.x);
        hash =
            HashDouble(
                hash,
                axis.y);
        hash =
            HashDouble(
                hash,
                axis.z);
    }

    return hash;
}

[[nodiscard]] u64 RuntimeOrbitFingerprint(
    const editor_session::EditorWorldSession& world,
    const scene::ObjectId bodyObject)
{
    const auto bodyId =
        world.Universe().
            BodyForObject(
                bodyObject);

    if (!bodyId.has_value())
        return 0U;

    const auto* body =
        world.Universe().
            Bodies().
            FindBody(
                *bodyId);

    if (body == nullptr)
        return 0U;

    u64 hash =
        0x4d33334f52424954ULL;

    hash =
        terrain::StableCombine64(
            hash,
            body->id.high);
    hash =
        terrain::StableCombine64(
            hash,
            body->id.low);

    hash =
        HashDouble(
            hash,
            universe::
                ReferenceRadiusMeters(
                    body->shape));

    if (body->mass.has_value())
    {
        hash =
            HashDouble(
                hash,
                body->mass->
                    massKilograms);
    }

    constexpr std::array<i64, 3>
        kTimes{
            0,
            3'600'000'000LL,
            86'400'000'000LL
        };

    for (const i64 micros :
         kTimes)
    {
        const auto transform =
            world.Universe().
                Frames().
                ResolveTransform(
                    body->centerFrame,
                    body->parentFrame,
                    time::SimulationTime{
                        .microsecondsFromEpoch =
                            micros});

        if (!transform.has_value())
            return 0U;

        hash =
            HashTransform(
                hash,
                *transform);
    }

    return hash;
}

[[nodiscard]] u64
DerivedAppearanceFingerprint(
    const editor_session::EditorWorldSession& world,
    const scene::ObjectId bodyObject)
{
    const auto bodyId =
        world.Universe().
            BodyForObject(
                bodyObject);

    if (!bodyId.has_value())
        return 0U;

    const auto* body =
        world.Universe().
            Bodies().
            FindBody(
                *bodyId);

    if (body == nullptr)
        return 0U;

    u64 hash =
        0x4d33334150504541ULL;

    const f64 referenceRadius =
        universe::
            ReferenceRadiusMeters(
                body->shape);

    const auto* terrainSurface =
        world.Surfaces().
            Registry().
            FindTerrainSurface(
                *bodyId);

    if (terrainSurface != nullptr &&
        terrainSurface->terrain != nullptr)
    {
        const auto planetaryAppearance =
            celestial_appearance::
                BuildPlanetaryAppearance(
                    *terrainSurface->terrain,
                    referenceRadius,
                    {.faceResolution = 17U,
                     .footprintScale = 1.5});

        hash =
            terrain::StableCombine64(
                hash,
                planetaryAppearance.
                    fingerprint);
    }

    if (const auto giant =
            world_model::
                ResolveGiantAppearance(
                    world.Objects(),
                    bodyObject);
        giant.has_value())
    {
        const auto product =
            celestial_giants::
                BuildGiantAppearance(
                    giant->parameters,
                    {.faceResolution = 17U});

        hash =
            terrain::StableCombine64(
                hash,
                product.fingerprint);
    }

    if (const auto small =
            world_model::
                ResolveSmallBodyAppearance(
                    world.Objects(),
                    bodyObject);
        small.has_value())
    {
        const auto appearance =
            celestial_small_bodies::
                BuildSmallBodyAppearance(
                    small->parameters,
                    {.faceResolution = 17U});

        const auto shape =
            celestial_small_bodies::
                BuildSmallBodyShape(
                    small->parameters,
                    {.faceResolution = 17U});

        hash =
            terrain::StableCombine64(
                hash,
                appearance.fingerprint);
        hash =
            terrain::StableCombine64(
                hash,
                shape.fingerprint);
    }

    if (const auto magnetosphere =
            world_model::
                ResolveMagnetosphere(
                    world.Objects(),
                    bodyObject,
                    referenceRadius);
        magnetosphere.has_value())
    {
        hash =
            terrain::StableCombine64(
                hash,
                magnetosphere->
                    fingerprint);

        hash =
            HashDouble(
                hash,
                celestial_magnetosphere::
                    MagnetopauseRadiusMeters(
                        magnetosphere->parameters,
                        referenceRadius,
                        {1.0, 0.0, 0.0}));
    }

    if (const auto compact =
            world_model::
                ResolveCompactObject(
                    world.Objects(),
                    bodyObject);
        compact.has_value())
    {
        const auto presentation =
            celestial_compact_objects::
                BuildCompactObjectPresentation(
                    compact->parameters);

        hash =
            terrain::StableCombine64(
                hash,
                presentation.fingerprint);

        if (const auto flow =
                world_model::
                    ResolveAccretionFlow(
                        world.Objects(),
                        bodyObject,
                        compact->parameters);
            flow.has_value())
        {
            hash =
                terrain::StableCombine64(
                    hash,
                    flow->fingerprint);
        }
    }

    hash =
        terrain::StableCombine64(
            hash,
            RuntimeOrbitFingerprint(
                world,
                bodyObject));

    return hash;
}

[[nodiscard]] u64 RepresentationFingerprint(
    const editor_session::EditorWorldSession& world,
    const scene::ObjectId bodyObject)
{
    const auto bodyId =
        world.Universe().
            BodyForObject(
                bodyObject);

    if (!bodyId.has_value())
        return 0U;

    const auto* body =
        world.Universe().
            Bodies().
            FindBody(
                *bodyId);

    if (body == nullptr)
        return 0U;

    const f64 radius =
        std::max(
            universe::
                ReferenceRadiusMeters(
                    body->shape),
            1.0);

    u64 hash =
        0x4d33335245505245ULL;

    constexpr std::array<f64, 5>
        kProjectedRadii{
            512.0,
            64.0,
            8.0,
            1.5,
            0.2
        };

    for (const f64 projected :
         kProjectedRadii)
    {
        constexpr f64 fov = 1.0;
        constexpr f64 height = 1000.0;

        const f64 angular =
            projected *
            fov /
            height;

        const f64 distance =
            radius /
            std::max(
                std::sin(angular),
                1.0e-9);

        const celestial_representation::
            ResolveInput input{
                .bodyRadiusMeters = radius,
                .maximumProductionDetailMeters =
                    0.0,
                .maximumMacroDisplacementMeters =
                    0.0,
                .cameraDistanceToCenterMeters =
                    distance,
                .verticalFieldOfViewRadians =
                    fov,
                .viewportHeightPixels =
                    height,
                .features = {
                    .productionSurfaceAvailable =
                        false,
                    .macroDisplacementAvailable =
                        false,
                    .complexFarAppearance =
                        false,
                    .radiativeEmitter =
                        false
                }
            };

        const auto decision =
            celestial_representation::
                Resolve(input);

        const auto blend =
            celestial_representation::
                ResolveRepresentationBlend(
                    input,
                    decision);

        hash =
            terrain::StableCombine64(
                hash,
                static_cast<u64>(
                    decision.representation));
        hash =
            terrain::StableCombine64(
                hash,
                static_cast<u64>(
                    decision.
                        lowerFidelityNeighbor));
        hash =
            HashDouble(
                hash,
                decision.
                    projectedRadiusPixels);
        hash =
            HashDouble(
                hash,
                blend.lowerWeight);
    }

    return hash;
}

void Fail(
    StudioCelestialRoundTripReport& report,
    const std::string_view stage,
    const std::string_view diagnostic)
{
    report.success = false;
    report.failureStage =
        std::string(stage);
    report.diagnostic =
        std::string(diagnostic);
}

[[nodiscard]] StudioCelestialRoundTripReport
VerifyInWorkspace(
    StudioWorkspace& workspace,
    const scene::ObjectId semanticBody)
{
    StudioCelestialRoundTripReport report{};

    try
    {
        if (!workspace.HasProject() ||
            !workspace.Session().
                World().
                HasWorld())
        {
            Fail(
                report,
                "preflight",
                "No project/world is open.");
            return report;
        }

        auto& before =
            workspace.Session().
                World();

        const auto record =
            before.Objects().
                Find(
                    semanticBody);

        if (!record.has_value() ||
            record->type !=
                world_model::
                    kCelestialBodyType)
        {
            Fail(
                report,
                "preflight",
                "Target object is not a Celestial Body.");
            return report;
        }

        report.projectManifest =
            workspace.Project().
                ManifestPath();
        report.worldPath =
            before.ActiveWorld().
                relativePath;
        report.semanticBody =
            semanticBody;

        static_cast<void>(
            before.RefreshUniverseIfChanged());

        const auto semanticBefore =
            FingerprintSemanticSubtree(
                before,
                semanticBody);

        report.semanticFingerprintBefore =
            semanticBefore.full;
        report.runtimeOrbitFingerprintBefore =
            RuntimeOrbitFingerprint(
                before,
                semanticBody);
        report.derivedAppearanceFingerprintBefore =
            DerivedAppearanceFingerprint(
                before,
                semanticBody);
        report.representationFingerprintBefore =
            RepresentationFingerprint(
                before,
                semanticBody);

        if (report.semanticFingerprintBefore == 0U ||
            report.runtimeOrbitFingerprintBefore == 0U ||
            report.derivedAppearanceFingerprintBefore == 0U ||
            report.representationFingerprintBefore == 0U)
        {
            Fail(
                report,
                "capture-before",
                "Could not derive complete celestial fingerprints.");
            return report;
        }

        before.Checkpoint();
        workspace.Project().Save();

        workspace.CloseProject();
        workspace.OpenProject(
            report.projectManifest);

        auto& after =
            workspace.Session().
                World();

        if (!after.HasWorld() ||
            after.ActiveWorld().
                    relativePath !=
                report.worldPath)
        {
            after.OpenWorld(
                report.worldPath);
        }

        report.freshWorkspaceRecomposition =
            true;

        static_cast<void>(
            after.RefreshUniverseIfChanged());

        const auto semanticAfter =
            FingerprintSemanticSubtree(
                after,
                semanticBody);

        report.semanticFingerprintAfter =
            semanticAfter.full;
        report.runtimeOrbitFingerprintAfter =
            RuntimeOrbitFingerprint(
                after,
                semanticBody);
        report.derivedAppearanceFingerprintAfter =
            DerivedAppearanceFingerprint(
                after,
                semanticBody);
        report.representationFingerprintAfter =
            RepresentationFingerprint(
                after,
                semanticBody);

        report.semanticIdsPreserved =
            semanticBefore.ids ==
            semanticAfter.ids;

        report.provenancePreserved =
            semanticBefore.provenance ==
            semanticAfter.provenance;

        report.derivedProductsRegenerated =
            report.
                    derivedAppearanceFingerprintAfter !=
                0U &&
            report.
                    representationFingerprintAfter !=
                0U;

        if (!report.semanticIdsPreserved ||
            !report.provenancePreserved ||
            report.semanticFingerprintBefore !=
                report.semanticFingerprintAfter)
        {
            Fail(
                report,
                "verify-semantic",
                "Celestial semantic IDs, properties or provenance changed after reopen.");
            return report;
        }

        if (report.runtimeOrbitFingerprintBefore !=
            report.runtimeOrbitFingerprintAfter)
        {
            Fail(
                report,
                "verify-orbit",
                "Recomposed runtime orbit/frame fingerprint changed after reopen.");
            return report;
        }

        if (report.
                derivedAppearanceFingerprintBefore !=
            report.
                derivedAppearanceFingerprintAfter)
        {
            Fail(
                report,
                "verify-derived",
                "Regenerated celestial appearance fingerprint changed after reopen.");
            return report;
        }

        if (report.
                representationFingerprintBefore !=
            report.
                representationFingerprintAfter)
        {
            Fail(
                report,
                "verify-representation",
                "Cross-representation resolver fingerprint changed after reopen.");
            return report;
        }

        report.success = true;
        report.diagnostic =
            "Celestial IDs, provenance, runtime orbit state and regenerated representation/appearance fingerprints are equivalent after a fresh workspace reopen.";
        return report;
    }
    catch (const std::exception& exception)
    {
        Fail(
            report,
            report.failureStage.empty()
                ? "exception"
                : report.failureStage,
            exception.what());
        return report;
    }
}
} // namespace

StudioCelestialRoundTripReport
VerifyStudioCelestialRoundTrip(
    StudioWorkspace& workspace,
    const scene::ObjectId semanticBody)
{
    return VerifyInWorkspace(
        workspace,
        semanticBody);
}

StudioCelestialRoundTripReport
VerifyStudioCelestialRoundTrip(
    documents::ProjectDocument& project,
    StudioSession& session,
    const scene::ObjectId semanticBody)
{
    StudioCelestialRoundTripReport report{};

    try
    {
        if (!session.World().HasWorld())
        {
            Fail(
                report,
                "preflight",
                "No world is open.");
            return report;
        }

        session.World().Checkpoint();
        project.Save();

        StudioWorkspace isolated;
        isolated.OpenProject(
            project.ManifestPath());

        if (isolated.Session().
                World().
                ActiveWorld().
                relativePath !=
            session.World().
                ActiveWorld().
                relativePath)
        {
            isolated.Session().
                OpenWorld(
                    session.World().
                        ActiveWorld().
                        relativePath);
        }

        report =
            VerifyInWorkspace(
                isolated,
                semanticBody);

        return report;
    }
    catch (const std::exception& exception)
    {
        Fail(
            report,
            report.failureStage.empty()
                ? "exception"
                : report.failureStage,
            exception.what());
        return report;
    }
}
} // namespace orbit::studio_session

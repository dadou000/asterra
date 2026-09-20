#include <orbit/studio_session/StudioTerrainRoundTripVerifier.hpp>

#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/terrain_debug/TerrainDebugField.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace orbit::studio_session
{
namespace
{
[[nodiscard]] u64 HashText(
    u64 seed,
    const std::string_view text) noexcept
{
    seed =
        terrain::StableCombine64(
            seed,
            static_cast<u64>(
                text.size()));

    for (const unsigned char value :
         text)
    {
        seed =
            terrain::StableCombine64(
                seed,
                static_cast<u64>(
                    value));
    }

    return seed;
}

[[nodiscard]] u64 HashDouble(
    const u64 seed,
    const f64 value) noexcept
{
    return terrain::StableCombine64(
        seed,
        std::bit_cast<u64>(value));
}

[[nodiscard]] u64 HashFloat(
    const u64 seed,
    const f32 value) noexcept
{
    return terrain::StableCombine64(
        seed,
        static_cast<u64>(
            std::bit_cast<u32>(
                value)));
}

[[nodiscard]] u64 HashProperty(
    u64 seed,
    const schema::PropertyValue& value)
{
    seed =
        terrain::StableCombine64(
            seed,
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
                seed =
                    terrain::StableCombine64(
                        seed,
                        typed ? 1U : 0U);
            }
            else if constexpr (
                std::is_same_v<T, i64>)
            {
                seed =
                    terrain::StableCombine64(
                        seed,
                        std::bit_cast<u64>(
                            typed));
            }
            else if constexpr (
                std::is_same_v<T, f64>)
            {
                seed =
                    HashDouble(
                        seed,
                        typed);
            }
            else if constexpr (
                std::is_same_v<
                    T,
                    std::string>)
            {
                seed =
                    HashText(
                        seed,
                        typed);
            }
            else if constexpr (
                std::is_same_v<
                    T,
                    math::Double3>)
            {
                seed =
                    HashDouble(
                        seed,
                        typed.x);
                seed =
                    HashDouble(
                        seed,
                        typed.y);
                seed =
                    HashDouble(
                        seed,
                        typed.z);
            }
            else if constexpr (
                std::is_same_v<
                    T,
                    schema::
                        ObjectReferenceValue>)
            {
                seed =
                    terrain::StableCombine64(
                        seed,
                        typed.high);
                seed =
                    terrain::StableCombine64(
                        seed,
                        typed.low);
            }
        },
        value);

    return seed;
}

[[nodiscard]] std::vector<scene::ObjectRecord>
CollectSubtree(
    const scene::ObjectStore& objects,
    const scene::ObjectId root)
{
    const auto rootRecord =
        objects.Find(root);

    if (!rootRecord.has_value())
    {
        return {};
    }

    std::vector<scene::ObjectRecord>
        result;

    std::vector<scene::ObjectRecord>
        pending{
            *rootRecord
        };

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

[[nodiscard]] u64 SemanticSubtreeFingerprint(
    const editor_session::EditorWorldSession& world,
    const scene::ObjectId root)
{
    const auto objects =
        CollectSubtree(
            world.Objects(),
            root);

    if (objects.empty())
    {
        return 0U;
    }

    u64 hash =
        0x4D313453454D414EULL; // "M14SEMAN"

    for (const auto& object :
         objects)
    {
        hash =
            terrain::StableCombine64(
                hash,
                object.id.high);
        hash =
            terrain::StableCombine64(
                hash,
                object.id.low);

        hash =
            terrain::StableCombine64(
                hash,
                object.parent.has_value()
                    ? object.parent->high
                    : 0U);
        hash =
            terrain::StableCombine64(
                hash,
                object.parent.has_value()
                    ? object.parent->low
                    : 0U);

        hash =
            terrain::StableCombine64(
                hash,
                object.type.high);
        hash =
            terrain::StableCombine64(
                hash,
                object.type.low);

        hash =
            terrain::StableCombine64(
                hash,
                std::bit_cast<u64>(
                    object.sortOrder));

        hash =
            HashText(
                hash,
                object.name);

        const auto* type =
            world.Schemas().
                FindType(
                    object.type);

        if (type == nullptr)
        {
            continue;
        }

        auto properties =
            type->properties;

        std::sort(
            properties.begin(),
            properties.end(),
            [](const schema::PropertySchema& a,
               const schema::PropertySchema& b)
            {
                return a.id < b.id;
            });

        for (const auto& property :
             properties)
        {
            hash =
                terrain::StableCombine64(
                    hash,
                    property.id.high);
            hash =
                terrain::StableCombine64(
                    hash,
                    property.id.low);

            const auto authored =
                world.Objects().
                    GetProperty(
                        object.id,
                        property.id);

            hash =
                HashProperty(
                    hash,
                    authored.has_value()
                        ? *authored
                        : property.defaultValue);
        }
    }

    return hash;
}

[[nodiscard]] u64 PhysicalSnapshotFingerprint(
    const StudioTerrainPhysicalPageSnapshot& snapshot)
{
    if (snapshot.material == nullptr ||
        snapshot.debugPage == nullptr)
    {
        return 0U;
    }

    u64 hash =
        0x4D31345048595349ULL; // "M14PHYSI"

    hash =
        terrain::StableCombine64(
            hash,
            snapshot.address.
                planet.high);
    hash =
        terrain::StableCombine64(
            hash,
            snapshot.address.
                planet.low);
    hash =
        terrain::StableCombine64(
            hash,
            static_cast<u64>(
                snapshot.address.
                    tile.face));
    hash =
        terrain::StableCombine64(
            hash,
            snapshot.address.
                tile.level);
    hash =
        terrain::StableCombine64(
            hash,
            snapshot.address.
                tile.x);
    hash =
        terrain::StableCombine64(
            hash,
            snapshot.address.
                tile.y);
    hash =
        terrain::StableCombine64(
            hash,
            snapshot.physicalLod);

    hash =
        terrain::StableCombine64(
            hash,
            snapshot.material->
                Resolution());

    hash =
        HashDouble(
            hash,
            snapshot.material->
                SpacingMeters());

    for (const auto& cell :
         snapshot.material->
             Cells())
    {
        hash =
            HashFloat(
                hash,
                cell.
                    bedrockHeightMeters);
        hash =
            HashFloat(
                hash,
                cell.
                    referenceBedrockHeightMeters);
        hash =
            terrain::StableCombine64(
                hash,
                cell.
                    bedrockMaterial.high);
        hash =
            terrain::StableCombine64(
                hash,
                cell.
                    bedrockMaterial.low);
        hash =
            HashFloat(
                hash,
                cell.regolithMeters);
        hash =
            HashFloat(
                hash,
                cell.soilMeters);
        hash =
            HashFloat(
                hash,
                cell.sandMeters);
        hash =
            HashFloat(
                hash,
                cell.debrisMeters);
        hash =
            HashFloat(
                hash,
                cell.moisture);
        hash =
            HashFloat(
                hash,
                cell.temporaryScalar);
    }

    for (const auto& descriptor :
         terrain_debug::FieldCatalog())
    {
        if (descriptor.field ==
                terrain_debug::
                    TerrainDebugField::
                        CacheResidency ||
            descriptor.field ==
                terrain_debug::
                    TerrainDebugField::
                        CacheInvalidation)
        {
            continue;
        }

        hash =
            terrain::StableCombine64(
                hash,
                static_cast<u64>(
                    descriptor.field));

        if (!snapshot.debugPage->
                Has(
                    descriptor.field))
        {
            hash =
                terrain::StableCombine64(
                    hash,
                    0U);
            continue;
        }

        hash =
            terrain::StableCombine64(
                hash,
                1U);

        const auto view =
            snapshot.debugPage->
                View(
                    descriptor.field);

        hash =
            terrain::StableCombine64(
                hash,
                view.width);
        hash =
            terrain::StableCombine64(
                hash,
                view.height);

        for (const auto value :
             view.scalar)
        {
            hash =
                HashFloat(
                    hash,
                    value);
        }

        for (const auto& value :
             view.vector)
        {
            hash =
                HashFloat(
                    hash,
                    value.x);
            hash =
                HashFloat(
                    hash,
                    value.y);
        }

        for (const auto value :
             view.category)
        {
            hash =
                terrain::StableCombine64(
                    hash,
                    value);
        }

        for (const auto value :
             view.boolean)
        {
            hash =
                terrain::StableCombine64(
                    hash,
                    value);
        }

        for (const auto value :
             view.lod)
        {
            hash =
                terrain::StableCombine64(
                    hash,
                    value);
        }

        // Revision-class debug channels are intentionally excluded from the
        // regenerated result comparison. M14 verifies authority content and
        // physical output, not session-local invalidation counters.
    }

    return hash;
}

[[nodiscard]] std::shared_ptr<
    const StudioTerrainPhysicalPageSnapshot>
DrivePageReady(
    StudioSession& session,
    const terrain::PhysicalTerrainPageAddress& address)
{
    constexpr u32 kMaximumIterations =
        40'000U;

    for (u32 iteration = 0U;
         iteration <
             kMaximumIterations;
         ++iteration)
    {
        static_cast<void>(
            session.Tick(false));

        session.
            TerrainPhysicalPages().
            RebuildDirty();

        const auto status =
            session.
                TerrainPhysicalPages().
                PageStatus(
                    address);

        const auto page =
            session.
                TerrainPhysicalPages().
                Find(
                    address);

        if (status.has_value() &&
            status->state ==
                TerrainRebuildState::
                    Ready &&
            page != nullptr)
        {
            return page;
        }

        std::this_thread::yield();
    }

    return nullptr;
}

void Fail(
    StudioTerrainRoundTripReport& report,
    const std::string_view stage,
    const std::string_view diagnostic)
{
    report.success = false;
    report.failureStage =
        std::string(stage);
    report.diagnostic =
        std::string(diagnostic);
}
} // namespace

StudioTerrainRoundTripReport
VerifyStudioTerrainRoundTrip(
    StudioWorkspace& workspace,
    const std::string_view viewportId)
{
    StudioTerrainRoundTripReport report{};

    try
    {
        if (!workspace.HasProject())
        {
            Fail(
                report,
                "preflight",
                "No Orbit Studio project is open.");
            return report;
        }

        auto& beforeSession =
            workspace.Session();

        if (!beforeSession.World().
                HasWorld())
        {
            Fail(
                report,
                "preflight",
                "No world is open.");
            return report;
        }

        static_cast<void>(
            beforeSession.Tick(false));

        const auto runtime =
            beforeSession.
                TerrainRuntime().
                Capture(
                    viewportId);

        if (!runtime.has_value())
        {
            Fail(
                report,
                "preflight",
                "The selected verification viewport has no production terrain runtime.");
            return report;
        }

        report.projectManifest =
            workspace.Project().
                ManifestPath();

        report.worldPath =
            beforeSession.World().
                ActiveWorld().
                relativePath;

        report.semanticBody =
            runtime->semanticBody;
        report.terrainObject =
            runtime->terrainObject;
        report.comparisonPage =
            runtime->
                observerPhysicalPage;

        report.semanticFingerprintBefore =
            SemanticSubtreeFingerprint(
                beforeSession.World(),
                report.semanticBody);

        if (report.
                semanticFingerprintBefore ==
            0U)
        {
            Fail(
                report,
                "capture-authority",
                "Could not fingerprint the selected semantic body subtree.");
            return report;
        }

        const auto beforePage =
            DrivePageReady(
                beforeSession,
                report.
                    comparisonPage);

        if (beforePage == nullptr)
        {
            Fail(
                report,
                "capture-physical",
                "The selected physical page did not reach Ready before the round trip.");
            return report;
        }

        report.physicalFingerprintBefore =
            PhysicalSnapshotFingerprint(
                *beforePage);

        if (report.
                physicalFingerprintBefore ==
            0U)
        {
            Fail(
                report,
                "capture-physical",
                "The selected physical page did not expose complete M12/M29 products.");
            return report;
        }

        const world::WorldPosition
            observer =
                runtime->observer;

        workspace.CloseProject();

        workspace.OpenProject(
            report.projectManifest);

        auto& afterSession =
            workspace.Session();

        if (!afterSession.World().
                HasWorld() ||
            afterSession.World().
                    ActiveWorld().
                    relativePath !=
                report.worldPath)
        {
            afterSession.OpenWorld(
                report.worldPath);
        }

        static_cast<void>(
            afterSession.Tick(false));

        const auto bodyRecord =
            afterSession.World().
                Objects().
                Find(
                    report.semanticBody);

        const auto terrainRecord =
            afterSession.World().
                Objects().
                Find(
                    report.terrainObject);

        report.semanticIdsPreserved =
            bodyRecord.has_value() &&
            terrainRecord.has_value();

        if (!report.semanticIdsPreserved)
        {
            Fail(
                report,
                "verify-authority",
                "Semantic body or terrain ObjectId changed after reopen.");
            return report;
        }

        report.semanticFingerprintAfter =
            SemanticSubtreeFingerprint(
                afterSession.World(),
                report.semanticBody);

        if (report.
                semanticFingerprintAfter !=
            report.
                semanticFingerprintBefore)
        {
            Fail(
                report,
                "verify-authority",
                "Authored semantic fingerprint changed after reopen.");
            return report;
        }

        const auto runtimeBody =
            afterSession.World().
                Surfaces().
                BodyForTerrainObject(
                    report.
                        terrainObject);

        if (!runtimeBody.has_value())
        {
            Fail(
                report,
                "verify-derived-reset",
                "Reopened Terrain Surface did not compose a runtime body.");
            return report;
        }

        const auto* services =
            afterSession.World().
                Surfaces().
                ServicesForBody(
                    *runtimeBody);

        if (services == nullptr)
        {
            Fail(
                report,
                "verify-derived-reset",
                "Reopened terrain has no TerrainBodyServices.");
            return report;
        }

        const auto cacheStats =
            services->Cache().
                Stats();

        report.derivedCacheFreshAfterReopen =
            cacheStats.residentPages ==
                0U &&
            cacheStats.residentBytes ==
                0U;

        report.debugResidencyFreshAfterReopen =
            afterSession.
                TerrainDebugPages().
                Size() == 0U;

        if (!report.
                derivedCacheFreshAfterReopen ||
            !report.
                debugResidencyFreshAfterReopen)
        {
            Fail(
                report,
                "verify-derived-reset",
                "Derived M26 cache or M29 residency survived project reopen.");
            return report;
        }

        constexpr std::string_view
            kVerificationViewport =
                "m14.terrain.verify";

        afterSession.
            Viewports().
            Register(
                std::string(
                    kVerificationViewport),
                ViewportMode::Perspective,
                false);

        afterSession.
            Viewports().
            PinToObject(
                kVerificationViewport,
                report.semanticBody);

        static_cast<void>(
            afterSession.Tick(false));

        if (!afterSession.
                TerrainRuntime().
                SetObserver(
                    kVerificationViewport,
                    observer))
        {
            static_cast<void>(
                afterSession.
                    Viewports().
                    Unregister(
                        kVerificationViewport));

            Fail(
                report,
                "regenerate",
                "Could not restore the comparison observer after reopen.");
            return report;
        }

        static_cast<void>(
            afterSession.Tick(false));

        const auto afterRuntime =
            afterSession.
                TerrainRuntime().
                Capture(
                    kVerificationViewport);

        if (!afterRuntime.has_value())
        {
            static_cast<void>(
                afterSession.
                    Viewports().
                    Unregister(
                        kVerificationViewport));

            Fail(
                report,
                "regenerate",
                "Verification viewport did not bind production terrain after reopen.");
            return report;
        }

        report.comparisonPagePreserved =
            afterRuntime->
                observerPhysicalPage ==
            report.comparisonPage;

        if (!report.
                comparisonPagePreserved)
        {
            static_cast<void>(
                afterSession.
                    Viewports().
                    Unregister(
                        kVerificationViewport));

            Fail(
                report,
                "regenerate",
                "Restored observer resolved to a different physical page.");
            return report;
        }

        const auto afterPage =
            DrivePageReady(
                afterSession,
                report.comparisonPage);

        report.physicalFingerprintAfter =
            afterPage != nullptr
                ? PhysicalSnapshotFingerprint(
                      *afterPage)
                : 0U;

        static_cast<void>(
            afterSession.
                Viewports().
                Unregister(
                    kVerificationViewport));

        if (afterPage == nullptr)
        {
            Fail(
                report,
                "regenerate",
                "Comparison page did not reach Ready after reopen.");
            return report;
        }

        if (report.
                physicalFingerprintAfter !=
            report.
                physicalFingerprintBefore)
        {
            Fail(
                report,
                "compare-physical",
                "Regenerated physical/M29 fingerprint differs from the pre-close result.");
            return report;
        }

        report.success = true;
        report.failureStage.clear();
        report.diagnostic =
            "Authored IDs/content and regenerated physical terrain are equivalent; derived cache/debug residency restarted fresh.";

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

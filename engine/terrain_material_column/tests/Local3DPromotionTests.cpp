#include <orbit/terrain_material_column/Local3DPromotion.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
using namespace orbit;

[[noreturn]] void Fail(const std::string& message)
{
    std::cerr << "M24 failure: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

void Require(
    const bool condition,
    const std::string& message)
{
    if (!condition)
    {
        Fail(message);
    }
}

void RequireNear(
    const f64 a,
    const f64 b,
    const f64 tolerance,
    const std::string& message)
{
    if (std::abs(a - b) > tolerance)
    {
        Fail(message);
    }
}

terrain_geology::RockTypeId TestRock()
{
    return {
        .high = 0x4F524249544D3234ULL,
        .low = 0x0000000000000001ULL
    };
}

terrain::PhysicalTerrainPageAddress TestAddress()
{
    return {
        .planet = {
            .high = 0x4F524249544D3234ULL,
            .low = 0x0000000000001000ULL
        },
        .tile = {
            .face = world::CubeFace::PositiveX,
            .level = 5,
            .x = 11,
            .y = 9
        }
    };
}

terrain_material_column::MaterialColumnPage
MakePage()
{
    terrain_material_column::MaterialColumnPage page(
        9,
        1.0);

    const auto rock = TestRock();

    for (u32 y = 0; y < page.Resolution(); ++y)
    {
        for (u32 x = 0; x < page.Resolution(); ++x)
        {
            const f32 bedrock =
                100.0F +
                static_cast<f32>(x) * 0.1F +
                static_cast<f32>(y) * 0.05F;

            page.SetCell(
                x,
                y,
                {
                    .bedrockHeightMeters = bedrock,
                    .referenceBedrockHeightMeters = bedrock,
                    .bedrockMaterial = rock,
                    .regolithMeters = 0.0F,
                    .soilMeters = 0.25F,
                    .sandMeters = 0.0F,
                    .debrisMeters = 0.0F,
                    .moisture = 0.25F,
                    .temporaryScalar = 0.0F
                });
        }
    }

    return page;
}

terrain_material_column::Local3DPromotionRequest
ExplicitRequest()
{
    return {
        .address = TestAddress(),
        .reason =
            terrain_material_column::
                Local3DPromotionReason::ExplicitAuthoring,
        .sourceRevision = 5,
        .promotionRevision = 2,
        .voidHeightMeters = 0.0,
        .undercutMeters = 0.0,
        .persistenceMeters = 0.0,
        .processConfidence = 0.0F
    };
}

void TestNormalTerrainAllocatesNoPromotion()
{
    const auto page = MakePage();

    auto request = ExplicitRequest();
    request.reason =
        terrain_material_column::
            Local3DPromotionReason::ProcessUndercut;
    request.voidHeightMeters = 0.4;
    request.undercutMeters = 0.1;
    request.persistenceMeters = 1.0;
    request.processConfidence = 0.99F;

    const auto promoted =
        terrain_material_column::TryPromoteLocal3D(
            page,
            request);

    Require(
        promoted == nullptr,
        "Ordinary/weak terrain must not allocate a persistent 3D region.");
}

void TestExplicitPromotionBuildsSingleSpanBaseline()
{
    const auto page = MakePage();
    auto promoted =
        terrain_material_column::TryPromoteLocal3D(
            page,
            ExplicitRequest());

    Require(
        promoted != nullptr,
        "Explicit authoring must be allowed to promote local terrain.");

    Require(
        promoted->Resolution() == page.Resolution(),
        "Promoted resolution must match the physical source page.");

    Require(
        promoted->BoundaryMatches(page),
        "Fresh promotion boundary must exactly match normal terrain.");

    for (u32 y = 0; y < page.Resolution(); ++y)
    {
        for (u32 x = 0; x < page.Resolution(); ++x)
        {
            const auto& cell = promoted->At(x, y);
            Require(
                cell.solidSpans.size() == 1U,
                "Promotion baseline must start as one solid span per cell.");

            const auto top =
                cell.TopSurfaceHeightMeters();
            Require(
                top.has_value(),
                "Baseline promoted cell must expose a top surface.");

            RequireNear(
                *top,
                page.At(x, y).SurfaceHeightMeters(),
                1.0e-5,
                "Promoted baseline top must equal M08 surface height.");
        }
    }
}

void TestProcessPromotionUsesStrictEvidence()
{
    const auto page = MakePage();

    auto request = ExplicitRequest();
    request.reason =
        terrain_material_column::
            Local3DPromotionReason::UndercutCliff;
    request.voidHeightMeters = 2.0;
    request.undercutMeters = 1.0;
    request.persistenceMeters = 8.0;
    request.processConfidence = 0.70F;

    Require(
        terrain_material_column::TryPromoteLocal3D(
            page,
            request) == nullptr,
        "Low-confidence process evidence must not promote terrain.");

    request.processConfidence = 0.95F;

    const auto accepted =
        terrain_material_column::TryPromoteLocal3D(
            page,
            request);

    Require(
        accepted != nullptr,
        "Strong persistent undercut evidence must promote terrain.");
}

void TestInteriorVoidCreatesOverhangWithoutMovingTop()
{
    const auto page = MakePage();
    auto promoted =
        terrain_material_column::TryPromoteLocal3D(
            page,
            ExplicitRequest());

    Require(
        promoted != nullptr,
        "Test requires promoted region.");

    constexpr u32 x = 4;
    constexpr u32 y = 4;

    const f32 surface =
        page.At(x, y).SurfaceHeightMeters();

    const bool carved =
        promoted->CarveVoid(
            x,
            y,
            surface - 5.0F,
            surface - 2.0F);

    Require(
        carved,
        "Interior carve must create a local 3D void.");

    const auto& cell = promoted->At(x, y);

    Require(
        cell.solidSpans.size() == 2U,
        "A middle carve must split one solid column into two spans.");

    Require(
        !cell.ContainsSolid(surface - 3.0F),
        "Carved interval must be a real void.");

    Require(
        cell.ContainsSolid(surface - 1.0F),
        "Rock above the void must remain as an overhang roof.");

    const auto top = cell.TopSurfaceHeightMeters();
    Require(
        top.has_value(),
        "Overhang must retain a top surface.");

    RequireNear(
        *top,
        surface,
        1.0e-5,
        "Carving a cave below the surface must not change macro top height.");
}

void TestBoundaryGuardCannotBeCarved()
{
    const auto page = MakePage();
    auto promoted =
        terrain_material_column::TryPromoteLocal3D(
            page,
            ExplicitRequest());

    Require(
        promoted != nullptr,
        "Test requires promoted region.");

    const f32 surface =
        page.At(0, 4).SurfaceHeightMeters();

    Require(
        !promoted->CarveVoid(
            0,
            4,
            surface - 4.0F,
            surface - 1.0F),
        "M24 must reject local 3D edits in the boundary guard ring.");

    Require(
        promoted->BoundaryMatches(page),
        "Interior promotion must retain a stable normal-terrain boundary.");
}

void TestDisconnectedSolidCanRepresentArch()
{
    const auto page = MakePage();
    auto promoted =
        terrain_material_column::TryPromoteLocal3D(
            page,
            ExplicitRequest());

    Require(
        promoted != nullptr,
        "Test requires promoted region.");

    constexpr u32 x = 4;
    constexpr u32 y = 4;

    const f32 surface =
        page.At(x, y).SurfaceHeightMeters();

    Require(
        promoted->AddSolid(
            x,
            y,
            {
                .bottomHeightMeters = surface + 2.0F,
                .topHeightMeters = surface + 4.0F,
                .material = TestRock()
            }),
        "Interior promotion must allow disconnected structural rock.");

    const auto& cell = promoted->At(x, y);

    Require(
        cell.solidSpans.size() == 2U,
        "Disconnected upper rock must form a second vertical span.");

    Require(
        !cell.ContainsSolid(surface + 1.0F) &&
        cell.ContainsSolid(surface + 3.0F),
        "Gap below added span must remain empty arch space.");
}

void TestPromotionIdentityIsStableAndRevisioned()
{
    const terrain_material_column::Local3DPromotionPolicy policy;
    auto request = ExplicitRequest();

    const u64 first =
        terrain_material_column::Local3DPromotionFingerprint(
            request,
            policy);
    const u64 second =
        terrain_material_column::Local3DPromotionFingerprint(
            request,
            policy);

    Require(
        first != 0 && first == second,
        "Same physical request/policy must reproduce the same promotion key.");

    ++request.promotionRevision;

    Require(
        first !=
            terrain_material_column::Local3DPromotionFingerprint(
                request,
                policy),
        "Promotion revision must explicitly invalidate local 3D identity.");
}
} // namespace

int main()
{
    TestNormalTerrainAllocatesNoPromotion();
    TestExplicitPromotionBuildsSingleSpanBaseline();
    TestProcessPromotionUsesStrictEvidence();
    TestInteriorVoidCreatesOverhangWithoutMovingTop();
    TestBoundaryGuardCannotBeCarved();
    TestDisconnectedSolidCanRepresentArch();
    TestPromotionIdentityIsStableAndRevisioned();

    std::cout << "Orbit M24 local 3D promotion tests passed.\n";
    return EXIT_SUCCESS;
}

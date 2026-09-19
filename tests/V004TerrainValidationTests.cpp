#include <orbit/terrain_geology/GeologicalMaterial.hpp>
#include <orbit/terrain_geology/Stratigraphy.hpp>
#include <orbit/terrain_material_column/MaterialColumnPage.hpp>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#ifndef ORBIT_TERRAIN_GEOLOGY_REFERENCE_ASSET_DIR
#error ORBIT_TERRAIN_GEOLOGY_REFERENCE_ASSET_DIR must be defined for M30 validation.
#endif

namespace
{
using namespace orbit;

[[noreturn]] void Fail(const std::string& message)
{
    std::cerr << "V0.0.4 M30 validation failure: "
              << message << '\n';
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

bool NearlyEqual(
    const f64 a,
    const f64 b,
    const f64 epsilon = 1.0e-5)
{
    return std::abs(a - b) <= epsilon;
}

struct StratigraphyFixture
{
    terrain_geology::GeologicalMaterialLibrary materials;
    terrain_geology::CompiledStratigraphyProfile stratigraphy;
    world::PlanetDefinition planet;
    terrain::PlanetSurfacePosition anchor;
    f64 topMeters{0.0};

    StratigraphyFixture()
        : materials(
              terrain_geology::LoadGeologicalMaterialDirectory(
                  std::filesystem::path{
                      ORBIT_TERRAIN_GEOLOGY_REFERENCE_ASSET_DIR})),
          stratigraphy(
              terrain_geology::LoadStratigraphyProfileFile(
                  std::filesystem::path{
                      ORBIT_TERRAIN_GEOLOGY_REFERENCE_ASSET_DIR} /
                  "continental_shelf.orbitstratigraphy"),
              materials),
          planet{
              .radiusMeters = 6'000'000.0,
              .id = {
                  .high = 0x4D333056414C3031ULL,
                  .low = 0x5354524154410001ULL
              },
              .generationSeed = 0x303001ULL},
          anchor{
              .planet = planet.id,
              .unitDirection =
                  math::Normalize(
                      stratigraphy.Profile().
                          transform.anchorUnitDirection),
              .radialOffsetMeters = 0.0}
    {
        topMeters =
            stratigraphy.DeformedTopRadialOffsetMeters(
                planet,
                anchor);
    }

    [[nodiscard]] terrain_geology::StratigraphySample
    SampleBedrock(
        const terrain_material_column::MaterialColumnPage& page) const
    {
        auto position = anchor;
        position.radialOffsetMeters =
            static_cast<f64>(
                page.At(0U, 0U).
                    bedrockHeightMeters);

        return stratigraphy.SampleExposedLayer(
            planet,
            position);
    }

    [[nodiscard]] terrain_material_column::MaterialColumnPage
    MakePage() const
    {
        terrain_material_column::MaterialColumnPage page(
            1U,
            2.0);

        auto position = anchor;
        position.radialOffsetMeters =
            topMeters - 0.5;

        const auto sample =
            stratigraphy.SampleExposedLayer(
                planet,
                position);

        page.SetCell(
            0U,
            0U,
            {
                .bedrockHeightMeters =
                    static_cast<f32>(
                        position.radialOffsetMeters),
                .referenceBedrockHeightMeters =
                    static_cast<f32>(
                        position.radialOffsetMeters),
                .bedrockMaterial =
                    sample.primaryMaterial,
                .regolithMeters = 0.0F,
                .soilMeters = 0.0F,
                .sandMeters = 0.0F,
                .debrisMeters = 0.0F,
                .moisture = 0.0F
            });

        return page;
    }
};

void Test01StratigraphyExposure()
{
    using namespace terrain_geology;

    StratigraphyFixture fixture;

    auto first =
        fixture.MakePage();
    auto replay =
        fixture.MakePage();

    const auto initial =
        fixture.SampleBedrock(first);

    Require(
        initial.primaryMaterial ==
            reference_rock::VolcanicAsh &&
        initial.layerIndex == 0U &&
        !initial.basement,
        "M30-01 setup must begin inside the weak volcanic-ash layer.");

    const auto firstRemoval =
        first.Erode(
            0U,
            0U,
            2.0,
            fixture.materials);

    const auto replayRemoval =
        replay.Erode(
            0U,
            0U,
            2.0,
            fixture.materials);

    const auto middle =
        fixture.SampleBedrock(first);
    const auto middleReplay =
        fixture.SampleBedrock(replay);

    Require(
        NearlyEqual(
            firstRemoval.bedrockMeters,
            2.0) &&
        middle.primaryMaterial ==
            reference_rock::Sandstone &&
        middle.layerIndex == 1U &&
        !middle.basement,
        "M30-01 lowering the physical M08 bedrock surface through the first virtual boundary must expose sandstone.");

    Require(
        first.At(0U, 0U).
                bedrockHeightMeters ==
            replay.At(0U, 0U).
                bedrockHeightMeters &&
        firstRemoval.removedMassKg ==
            replayRemoval.removedMassKg &&
        middle.primaryMaterial ==
            middleReplay.primaryMaterial &&
        middle.layerIndex ==
            middleReplay.layerIndex,
        "M30-01 stratigraphy exposure must be deterministic for identical physical erosion.");

    const auto deepRemoval =
        first.Erode(
            0U,
            0U,
            6.0,
            fixture.materials);

    const auto deep =
        fixture.SampleBedrock(first);

    Require(
        NearlyEqual(
            deepRemoval.bedrockMeters,
            6.0) &&
        deep.primaryMaterial ==
            reference_rock::Limestone &&
        deep.layerIndex == 2U &&
        !deep.basement,
        "M30-01 continued physical incision must advance into the limestone layer rather than remaining phase-locked to the original stratum.");

    Require(
        NearlyEqual(
            deep.depthBelowTopMeters,
            8.5,
            1.0e-4),
        "M30-01 virtual stratigraphic depth must track the eroded physical bedrock height.");
}
} // namespace

int main()
{
    Test01StratigraphyExposure();

    std::cout
        << "Orbit V0.0.4 M30 validation: 1/20 deterministic cases passed.\n";
    return EXIT_SUCCESS;
}

#include <orbit/terrain_material_column/SurfaceResolver.hpp>
#include <orbit/terrain_render/PhysicalSurface.hpp>
#include <orbit/terrain_scatter/PhysicalSurface.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
using namespace orbit;
using namespace orbit::terrain_material_column;

[[noreturn]] void Fail(
    const std::string& message)
{
    std::cerr
        << "M18 failure: "
        << message
        << '\n';

    std::exit(
        EXIT_FAILURE);
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
    if (std::abs(
            a - b) >
        tolerance)
    {
        Fail(
            message +
            " (" +
            std::to_string(a) +
            " vs " +
            std::to_string(b) +
            ")");
    }
}

terrain_geology::GeologicalMaterialLibrary
MakeGeology()
{
    terrain_geology::GeologicalMaterialLibrary geology;

    geology.Upsert({
        .id =
            terrain_geology::
                reference_rock::
                    Basalt,
        .name = "M18 basalt",
        .hardness = 0.92F,
        .cohesion = 0.88F,
        .hydraulicErodibility = 0.12F,
        .aeolianErodibility = 0.04F,
        .permeability = 0.08F,
        .chemicalWeatherability = 0.20F,
        .fractureTendency = 0.35F,
        .density = 3'000.0F
    });

    geology.Upsert({
        .id =
            terrain_geology::
                reference_rock::
                    Granite,
        .name = "M18 granite",
        .hardness = 0.95F,
        .cohesion = 0.93F,
        .hydraulicErodibility = 0.07F,
        .aeolianErodibility = 0.03F,
        .permeability = 0.04F,
        .chemicalWeatherability = 0.16F,
        .fractureTendency = 0.28F,
        .density = 2'700.0F
    });

    return geology;
}

MaterialColumnCell BasaltWithSoil()
{
    return {
        .bedrockHeightMeters = 100.0F,
        .referenceBedrockHeightMeters = 100.0F,
        .bedrockMaterial =
            terrain_geology::
                reference_rock::
                    Basalt,
        .regolithMeters = 0.20F,
        .soilMeters = 0.35F,
        .sandMeters = 0.0F,
        .debrisMeters = 0.0F,
        .moisture = 0.65F,
        .temporaryScalar = 0.0F
    };
}

void RequireConsumersMatch(
    const ExposedSurfaceState& state)
{
    const auto render =
        terrain_render::
            MakePhysicalSurfaceRenderInput(
                state);

    const auto scatter =
        terrain_scatter::
            MakePhysicalSurfaceScatterInput(
                state);

    Require(
        render.material ==
                state.material &&
            scatter.material ==
                state.material,
        "Renderer/scatter did not consume the canonical M18 material class.");

    Require(
        render.substrateRock ==
                state.substrateRock &&
            scatter.substrateRock ==
                state.substrateRock,
        "Renderer/scatter did not consume the canonical M18 substrate identity.");

    Require(
        render.exposedRock ==
                state.exposedRock &&
            scatter.exposedRock ==
                state.exposedRock,
        "Renderer/scatter independently changed exposed rock identity.");

    RequireNear(
        render.
            exposedLayerDepthMeters,
        state.
            exposedLayerDepthMeters,
        0.0,
        "Renderer changed canonical exposed-layer depth.");

    RequireNear(
        scatter.
            exposedLayerDepthMeters,
        state.
            exposedLayerDepthMeters,
        0.0,
        "Scatter changed canonical exposed-layer depth.");

    Require(
        render.BedrockExposed() ==
            scatter.BedrockExposed(),
        "Renderer/scatter disagree about physical bedrock exposure.");
}

void TestSoilStrippingExposesBasalt()
{
    auto geology =
        MakeGeology();

    MaterialColumnPage page(
        1U,
        1.0);

    page.SetCell(
        0U,
        0U,
        BasaltWithSoil());

    const auto geologySample =
        SampleColumnGeology(
            page.At(
                0U,
                0U),
            geology);

    const ProcessDerivedFields fields{
        .standingWaterDepthMeters =
            0.15F,
        .snowDepthMeters =
            0.0F
    };

    const auto initial =
        ResolveSurface(
            page.At(
                0U,
                0U),
            geologySample,
            fields);

    Require(
        initial.material ==
            ExposedSurfaceKind::Soil,
        "M18 did not report actual topmost soil.");

    Require(
        !initial.exposedRock.IsValid(),
        "Covered bedrock was incorrectly reported as the exposed material.");

    RequireConsumersMatch(
        initial);

    static_cast<void>(
        page.Erode(
            0U,
            0U,
            0.55,
            geology));

    const auto stripped =
        ResolveSurface(
            page.At(
                0U,
                0U),
            SampleColumnGeology(
                page.At(
                    0U,
                    0U),
                geology),
            fields);

    Require(
        stripped.material ==
            ExposedSurfaceKind::Bedrock,
        "Removing loose cover did not automatically expose bedrock.");

    Require(
        stripped.exposedRock ==
            terrain_geology::
                reference_rock::
                    Basalt,
        "Removing soil from basalt did not expose basalt identity.");

    Require(
        stripped.substrateRock ==
            terrain_geology::
                reference_rock::
                    Basalt,
        "M18 changed geological substrate identity while stripping loose cover.");

    RequireNear(
        stripped.
            exposedLayerDepthMeters,
        0.0,
        0.0,
        "Exposed bedrock cannot report a loose-layer thickness.");

    RequireConsumersMatch(
        stripped);
}

void TestSandDepositionBuriesRock()
{
    auto geology =
        MakeGeology();

    MaterialColumnPage page(
        1U,
        1.0);

    auto cell =
        BasaltWithSoil();

    cell.regolithMeters = 0.0F;
    cell.soilMeters = 0.0F;

    page.SetCell(
        0U,
        0U,
        cell);

    const auto before =
        ResolveSurface(
            page.At(
                0U,
                0U),
            SampleColumnGeology(
                page.At(
                    0U,
                    0U),
                geology));

    Require(
        before.BedrockExposed(),
        "M18 deposition test must begin on exposed rock.");

    static_cast<void>(
        page.Deposit(
            0U,
            0U,
            LooseMaterialKind::Sand,
            0.42));

    const auto after =
        ResolveSurface(
            page.At(
                0U,
                0U),
            SampleColumnGeology(
                page.At(
                    0U,
                    0U),
                geology));

    Require(
        after.material ==
            ExposedSurfaceKind::Sand,
        "Depositing sand did not automatically change M18 exposure to sand.");

    Require(
        !after.exposedRock.IsValid(),
        "Buried bedrock remained marked as physically exposed.");

    Require(
        after.substrateRock ==
            terrain_geology::
                reference_rock::
                    Basalt,
        "Sand deposition rewrote the underlying geological identity.");

    RequireNear(
        after.
            exposedLayerDepthMeters,
        0.42,
        1.0e-6,
        "M18 did not report physical sand cover depth.");

    RequireConsumersMatch(
        after);
}

void TestProcessFieldsCannotSelectMaterial()
{
    auto geology =
        MakeGeology();

    const auto column =
        BasaltWithSoil();

    const auto sample =
        SampleColumnGeology(
            column,
            geology);

    const auto dry =
        ResolveSurface(
            column,
            sample,
            {
                .standingWaterDepthMeters =
                    0.0F,
                .snowDepthMeters =
                    0.0F
            });

    const auto wetSnowy =
        ResolveSurface(
            column,
            sample,
            {
                .standingWaterDepthMeters =
                    5.0F,
                .snowDepthMeters =
                    2.0F
            });

    Require(
        dry.material ==
                wetSnowy.material &&
            dry.substrateRock ==
                wetSnowy.substrateRock &&
            dry.exposedRock ==
                wetSnowy.exposedRock,
        "M18 process-derived fields changed physical exposure identity.");

    RequireNear(
        wetSnowy.
            standingWaterDepthMeters,
        5.0,
        0.0,
        "M18 failed to forward process water state.");

    RequireNear(
        wetSnowy.
            snowDepthMeters,
        2.0,
        0.0,
        "M18 failed to forward process snow state.");
}

void TestMismatchedGeologyCannotOverrideColumn()
{
    auto geology =
        MakeGeology();

    const auto column =
        BasaltWithSoil();

    auto wrong =
        SampleColumnGeology(
            column,
            geology);

    const auto* granite =
        geology.Find(
            terrain_geology::
                reference_rock::
                    Granite);

    Require(
        granite != nullptr,
        "M18 geology fixture lost granite.");

    wrong.bedrockMaterial =
        granite->id;

    bool rejected = false;

    try
    {
        static_cast<void>(
            ResolveSurface(
                column,
                wrong));
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }

    Require(
        rejected,
        "M18 accepted a geology identity that disagrees with M08 substrate authority.");
}
} // namespace

int main()
{
    TestSoilStrippingExposesBasalt();
    TestSandDepositionBuriesRock();
    TestProcessFieldsCannotSelectMaterial();
    TestMismatchedGeologyCannotOverrideColumn();

    std::cout
        << "Orbit M18 exposed-surface resolver tests passed.\n";

    return EXIT_SUCCESS;
}

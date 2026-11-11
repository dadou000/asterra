#include <orbit/rhi/vulkan/VulkanBackend.hpp>
#include <orbit/shader/dxc/DxcShaderCompiler.hpp>
#include <orbit/terrain/BakedGeology.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/terrain_gpu/GpuGeologyCompiler.hpp>
#include <orbit/terrain_impacts/ImpactField.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <iostream>
#include <memory>
#include <vector>

namespace
{
using namespace orbit;

constexpr u32 kResolution = 64U;
constexpr u64 kRecipeHash = 0x47454F4C4F475931ULL;

[[nodiscard]] std::shared_ptr<const terrain::BakedGeologyRasters> BuildCpuReference(
    const world::PlanetDefinition& planet,
    const terrain_impacts::ImpactFieldDefinition& definition)
{
    const std::size_t stride = static_cast<std::size_t>(kResolution) + 2U;
    const std::size_t sampleCount = 6U * stride * stride;
    std::vector<f32> impactRelief(sampleCount, 0.0F);
    std::vector<f32> iceRelief(sampleCount, 0.0F);
    std::vector<terrain::BakedGeologyProcessTexel> process(sampleCount);
    terrain_impacts::ImpactField impacts(planet, definition);
    terrain_impacts::IceFractureField fractures(planet, *definition.iceFractures);
    terrain_impacts::ImpactQueryScratch scratch;
    const f64 footprint = 2.0 * planet.radiusMeters / kResolution;

    for (u32 face = 0U; face < terrain::BakedGeologyRasters::kFaces; ++face)
    {
        for (i32 y = -1; y <= static_cast<i32>(kResolution); ++y)
        {
            for (i32 x = -1; x <= static_cast<i32>(kResolution); ++x)
            {
                const std::size_t index =
                    (static_cast<std::size_t>(face) * stride +
                     static_cast<std::size_t>(y + 1)) * stride +
                    static_cast<std::size_t>(x + 1);
                const math::Double3 direction = terrain::BakedGeologyTexelDirection(
                    face, x, y, kResolution);
                auto impact = impacts.Sample(direction, footprint, scratch);
                const auto ice = fractures.Sample(direction, footprint, scratch);
                const bool fracturesAfterSurface = ice.damage > 0.0 &&
                    ice.ageOrder >= impact.exposureAgeOrder;
                const f64 fractureRetention = fracturesAfterSurface || ice.damage <= 0.0
                    ? 1.0
                    : std::clamp(1.0 - impact.resurfacedMaterialFraction, 0.0, 1.0) *
                        std::clamp(1.0 - impact.excavationCoverage, 0.0, 1.0);
                if (fracturesAfterSurface)
                {
                    impact.exposureAgeOrder = ice.ageOrder;
                    impact.exposureAgeYears = std::max(
                        0.0, definition.surfaceAgeYears - ice.formationAgeYears);
                }
                impactRelief[index] = static_cast<f32>(impact.heightDeltaMeters);
                iceRelief[index] = static_cast<f32>(ice.heightDeltaMeters * fractureRetention);
                process[index] = {
                    .excavationDepthMeters = static_cast<f32>(impact.excavationDepthMeters),
                    .ejectaThicknessMeters = static_cast<f32>(impact.ejectaThicknessMeters),
                    .debrisField = static_cast<f32>(impact.debrisField),
                    .rayField = static_cast<f32>(impact.rayField),
                    .meltThicknessMeters = static_cast<f32>(impact.meltThicknessMeters),
                    .brecciaField = static_cast<f32>(impact.brecciaField),
                    .resurfacedMaterialFraction = static_cast<f32>(impact.resurfacedMaterialFraction),
                    .resurfacingThicknessMeters = static_cast<f32>(impact.resurfacingThicknessMeters),
                    .microImpactRoughnessMeters = static_cast<f32>(impact.microImpactRoughnessMeters),
                    .microImpactCoverage = static_cast<f32>(impact.microImpactCoverage),
                    .excavationCoverage = static_cast<f32>(impact.excavationCoverage),
                    .formationAgeYears = static_cast<f32>(impact.formationAgeYears),
                    .exposureAgeYears = static_cast<f32>(impact.exposureAgeYears),
                    .formationAgeOrder = impact.formationAgeOrder,
                    .exposureAgeOrder = impact.exposureAgeOrder,
                    .affectingImpacts = impact.affectingImpacts,
                    .iceDamage = static_cast<f32>(ice.damage * fractureRetention),
                    .fractureCoverage = static_cast<f32>(ice.fractureCoverage * fractureRetention),
                    .nearbySegments = ice.nearbySegments};
            }
        }
    }

    return std::make_shared<const terrain::BakedGeologyRasters>(
        terrain::BakedGeologyRasters::Build(
            kResolution, kRecipeHash,
            std::move(impactRelief), std::move(iceRelief), std::move(process)));
}

[[nodiscard]] f64 Difference(const f32 left, const f32 right)
{
    return std::abs(static_cast<f64>(left) - static_cast<f64>(right));
}
} // namespace

int RunGeologyCompilerTest()
{
    const auto device = orbit::rhi::vulkan::CreateDevice({.enableValidation = true});
    const orbit::shader::dxc::DxcShaderCompiler shaderCompiler;
    const orbit::terrain_gpu::GpuGeologyCompiler compiler(*device, shaderCompiler);

    const orbit::world::PlanetDefinition planet{
        .radiusMeters = 6'000'000.0,
        .id = {.high = 0x47454F4C4F475931ULL, .low = 0xC0DEC0DEULL},
        .generationSeed = 0xA57E22AULL};
    const math::Double3 craterCenter = terrain::BakedGeologyTexelDirection(
        0U, static_cast<i32>(kResolution / 2U),
        static_cast<i32>(kResolution / 2U), kResolution);
    terrain_impacts::ImpactFieldDefinition history{
        .id = {.high = 7U, .low = 9U},
        .planet = planet.id,
        .name = "GPU geology parity",
        .seed = 0xA57E22AULL,
        .complexTransitionRadiusMeters = 200'000.0,
        .environment = terrain_impacts::SurfaceEnvironment::Icy,
        .surfaceAgeYears = 4.5e9,
        .surfaceGravityMetersPerSecondSquared = 3.71,
        .targetDensityKgPerCubicMeter = 2'900.0,
        .targetStrengthPascals = 1'500'000.0};
    history.authoredImpacts.push_back({
        .id = {.high = 13U, .low = 17U},
        .centerUnitDirection = craterCenter,
        .radiusMeters = 420'000.0,
        .profile = terrain_impacts::CraterProfileKind::Complex,
        .simpleDepthRatio = 0.16,
        .complexDepthRatio = 0.07,
        .rimHeightRatio = 0.04,
        .ejectaThicknessRatio = 0.02,
        .ejectaExtentRadii = 3.5,
        .rayStrength = 0.5,
        .rayCount = 8U,
        .rayExtentRadii = 12.0,
        .rayIrregularity = 0.2,
        .degradation = 0.2,
        .ageOrder = 0xfffffff0ULL,
        .formationAgeYears = 4.0e9,
        .impactAngleDegrees = 35.0,
        .impactAzimuthRadians = 0.4,
        .shapeIrregularity = 0.16,
        .meltFraction = 0.2,
        .brecciaFraction = 0.55,
        .multiringStrength = 0.25,
        .binarySeparationRadii = 0.2,
        .secondaryCount = 2U});
    auto ice = std::make_shared<terrain_impacts::IceFractureDefinition>();
    ice->seed = 42U;
    ice->ageOrder = 0xfffffffeULL;
    ice->formationAgeYears = 4.2e9;
    ice->fractureCount = 128U;
    ice->segmentsPerFracture = 10U;
    ice->maximumLengthMeters = 1'500'000.0;
    ice->widthMeters = 150'000.0;
    history.iceFractures = ice;
    const auto flowFrame = world::MakeSurfaceFrame(craterCenter);
    terrain_impacts::ResurfacingRecord flow{
        .id = {.high = 19U, .low = 23U},
        .kind = terrain_impacts::ResurfacingKind::TectonicRenewal,
        .centerlineUnitDirections = {
            world::DirectionAtSurfaceOffset(planet, flowFrame, {-700'000.0, 0.0}),
            world::DirectionAtSurfaceOffset(planet, flowFrame, {700'000.0, 0.0})},
        .widthMeters = 500'000.0,
        .thicknessMeters = 100.0,
        .formationAgeYears = 4.4e9,
        .ageOrder = 0x100000001ULL};
    history.resurfacingEvents.push_back(flow);
    flow.id.low += 1U;
    flow.kind = terrain_impacts::ResurfacingKind::LavaFlow;
    flow.widthMeters = 220'000.0;
    flow.formationAgeYears = history.surfaceAgeYears;
    flow.ageOrder += 1U;
    history.resurfacingEvents.push_back(flow);

    auto historyPointer =
        std::make_shared<const terrain_impacts::ImpactFieldDefinition>(history);
    terrain::AnalyticTerrainDesc desc{.seed = history.seed};
    desc.impactHistory = historyPointer;
    const std::function<bool()> notCancelled = [] { return false; };
    const auto product = compiler.Compile(
        planet, desc, {}, nullptr, kResolution, kRecipeHash,
        notCancelled, {});
    if (product.rasters == nullptr || product.dispatches == 0U ||
        product.samples != 6U * (kResolution + 2U) * (kResolution + 2U) ||
        !product.rasters->HasProcessChannels())
    {
        std::cerr << "GPU geology compiler did not produce the expected base rasters.\n";
        return 1;
    }

    const auto cpu = BuildCpuReference(planet, history);
    const auto& gpuProcess = product.rasters->ProcessGutter();
    const auto& cpuProcess = cpu->ProcessGutter();
    f64 maximumReliefDelta = 0.0;
    f64 maximumIceDelta = 0.0;
    f64 maximumProcessDelta = 0.0;
    const char* maximumProcessChannel = "none";
    u32 maximumNearbySegmentDelta = 0U;
    u32 nonzeroImpactSamples = 0U;
    u32 nonzeroFractureSamples = 0U;
    for (std::size_t index = 0U; index < cpuProcess.size(); ++index)
    {
        maximumReliefDelta = std::max(maximumReliefDelta,
            Difference(product.rasters->ReliefGutter()[index], cpu->ReliefGutter()[index]));
        maximumIceDelta = std::max(maximumIceDelta,
            Difference(product.rasters->IceReliefGutter()[index], cpu->IceReliefGutter()[index]));
        const auto& actual = gpuProcess[index];
        const auto& expected = cpuProcess[index];
        const auto recordProcessDelta = [&](const f64 delta, const char* channel)
        {
            if (delta > maximumProcessDelta)
            {
                maximumProcessDelta = delta;
                maximumProcessChannel = channel;
            }
        };
        recordProcessDelta(Difference(actual.excavationDepthMeters,
            expected.excavationDepthMeters), "excavation");
        recordProcessDelta(Difference(actual.ejectaThicknessMeters,
            expected.ejectaThicknessMeters), "ejecta");
        recordProcessDelta(Difference(actual.debrisField, expected.debrisField), "debris");
        recordProcessDelta(Difference(actual.rayField, expected.rayField), "rays");
        recordProcessDelta(Difference(actual.meltThicknessMeters,
            expected.meltThicknessMeters), "melt");
        recordProcessDelta(Difference(actual.brecciaField, expected.brecciaField), "breccia");
        recordProcessDelta(Difference(actual.resurfacedMaterialFraction,
            expected.resurfacedMaterialFraction), "resurfaced material");
        recordProcessDelta(Difference(actual.resurfacingThicknessMeters,
            expected.resurfacingThicknessMeters), "resurfacing thickness");
        recordProcessDelta(Difference(actual.iceDamage, expected.iceDamage), "ice damage");
        recordProcessDelta(Difference(actual.fractureCoverage,
            expected.fractureCoverage), "fracture coverage");
        if (actual.formationAgeOrder != expected.formationAgeOrder ||
            actual.exposureAgeOrder != expected.exposureAgeOrder ||
            actual.affectingImpacts != expected.affectingImpacts)
        {
            std::cerr << "GPU geology chronology/count output differs at " << index
                      << ": formation " << actual.formationAgeOrder << "/"
                      << expected.formationAgeOrder << ", exposure "
                      << actual.exposureAgeOrder << "/" << expected.exposureAgeOrder
                      << ", impacts " << actual.affectingImpacts << "/"
                      << expected.affectingImpacts << ", segments "
                      << actual.nearbySegments << "/" << expected.nearbySegments << '\n';
            return 2;
        }
        maximumNearbySegmentDelta = std::max(
            maximumNearbySegmentDelta,
            actual.nearbySegments > expected.nearbySegments
                ? actual.nearbySegments - expected.nearbySegments
                : expected.nearbySegments - actual.nearbySegments);
        if (std::abs(expected.excavationDepthMeters) > 0.01F) ++nonzeroImpactSamples;
        if (expected.nearbySegments > 0U) ++nonzeroFractureSamples;
    }

    std::cout << "GPU geology parity: relief=" << maximumReliefDelta
              << " m, ice=" << maximumIceDelta
              << " m, process=" << maximumProcessDelta << " (" << maximumProcessChannel << ")"
              << ", segment-count delta=" << maximumNearbySegmentDelta
              << ", impacts=" << nonzeroImpactSamples
              << ", fractured=" << nonzeroFractureSamples
              << ", dispatches=" << product.dispatches << '\n';
    if (maximumReliefDelta > 50.0 || maximumIceDelta > 20.0 ||
        maximumProcessDelta > 0.2 || nonzeroImpactSamples == 0U ||
        nonzeroFractureSamples == 0U || maximumNearbySegmentDelta > 16U)
    {
        std::cerr << "GPU geology parity exceeded its float32 tolerance or missed a feature.\n";
        return 3;
    }
    return 0;
}

int main()
{
    try
    {
        return RunGeologyCompilerTest();
    }
    catch (const std::exception& exception)
    {
        std::cerr << "GPU geology compiler test failed: " << exception.what() << '\n';
        return 99;
    }
}

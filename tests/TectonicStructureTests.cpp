#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/world/Planet.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>

namespace
{
using namespace orbit;

bool Check(const bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAILED: " << message << '\n';
    return condition;
}

math::Double3 Fibonacci(const u32 i, const u32 n)
{
    const f64 y = 1.0 - 2.0 * (static_cast<f64>(i) + 0.5) / static_cast<f64>(n);
    const f64 r = std::sqrt(std::max(0.0, 1.0 - y * y));
    const f64 a = std::numbers::pi * (3.0 - std::sqrt(5.0)) * static_cast<f64>(i);
    return {r * std::cos(a), y, r * std::sin(a)};
}

bool StructureIsBoundedAndDeterministic()
{
    const world::PlanetDefinition planet{.radiusMeters = 6'000'000.0};
    terrain::AnalyticTerrainDesc desc{.seed = 4242};
    const terrain::AnalyticTerrainSource source(planet, desc);
    const terrain::AnalyticTerrainSource repeated(planet, desc);

    bool ok = true;
    f64 oceanicMax = 0.0;
    f64 continentalMin = 1.0e9;
    f64 youngestAtDivergence = 1.0;
    f64 thickestAtCollision = 0.0;
    u32 convergent = 0, divergent = 0, transform = 0, none = 0;
    for (u32 i = 0; i < 20'000; ++i)
    {
        const auto d = Fibonacci(i, 20'000);
        const auto s = source.GlobalFields().SampleTectonicStructure(d);
        const auto t = repeated.GlobalFields().SampleTectonicStructure(d);
        ok &= Check(s.plateId == t.plateId && s.crustThicknessKm == t.crustThicknessKm &&
                s.upliftMeters == t.upliftMeters, "structure must be deterministic");
        ok &= Check(s.crustAge >= 0.0 && s.crustAge <= 1.0 &&
                s.geologicalAge >= 0.0 && s.geologicalAge <= 1.0, "age within 0..1");
        ok &= Check(s.stress >= 0.0 && s.stress <= 1.0 &&
                s.volcanism >= 0.0 && s.volcanism <= 1.0 &&
                s.boundaryStrength >= 0.0 && s.boundaryStrength <= 1.0, "unit fields within 0..1");
        ok &= Check(s.crustThicknessKm >= 3.0 && std::isfinite(s.crustThicknessKm) &&
                s.upliftMeters >= 0.0 && s.subsidenceMeters >= 0.0, "thickness/uplift sane");
        ok &= Check(s.plateId < terrain::kMaxTectonicPlates, "plate id in range");

        if (s.boundaryStrength < 0.05)
        {
            ++none;
            ok &= Check(s.boundaryType == terrain::TectonicBoundaryType::None, "interior has no boundary");
            // Crust type is its own field now, not the plate flag.
            if (s.continentalCrustFraction > 0.8) continentalMin = std::min(continentalMin, s.crustThicknessKm);
            else if (s.continentalCrustFraction < 0.2) oceanicMax = std::max(oceanicMax, s.crustThicknessKm);
        }
        switch (s.boundaryType)
        {
        case terrain::TectonicBoundaryType::Convergent:
            ++convergent;
            if (s.continental && s.neighbourContinental)
                thickestAtCollision = std::max(thickestAtCollision, s.crustThicknessKm);
            break;
        case terrain::TectonicBoundaryType::Divergent:
            ++divergent;
            if (s.divergence > 0.9) youngestAtDivergence = std::min(youngestAtDivergence, s.crustAge);
            break;
        case terrain::TectonicBoundaryType::Transform: ++transform; break;
        case terrain::TectonicBoundaryType::None: break;
        }
    }
    ok &= Check(none > 0 && convergent > 0 && divergent > 0 && transform > 0,
        "all boundary classes should occur on a 14-plate planet");
    ok &= Check(continentalMin > oceanicMax, "continental crust is thicker than oceanic");
    ok &= Check(youngestAtDivergence < 0.15, "spreading centres have newly formed crust");
    ok &= Check(thickestAtCollision > 50.0, "continental collision thickens crust");

    const auto zero = source.GlobalFields().SampleTectonicStructure({0.0, 0.0, 0.0});
    ok &= Check(std::isfinite(zero.crustThicknessKm), "zero direction falls back to a valid pole");
    return ok;
}
math::Double3 LatLonDirection(const f64 latitudeDegrees, const f64 longitudeDegrees)
{
    const f64 a = latitudeDegrees * std::numbers::pi / 180.0;
    const f64 b = longitudeDegrees * std::numbers::pi / 180.0;
    return {std::cos(a) * std::cos(b), std::sin(a), std::cos(a) * std::sin(b)};
}

// Mountain belts used to end in straight cuts that started at triple
// junctions: the boundary was evaluated against only the two nearest plates,
// so its normal and relative velocity jumped wherever the runner-up plate
// changed identity (convergence jumped by up to 1.0 across a quarter degree,
// ~99% of them with the same nearest plate). The masks must now vary smoothly
// everywhere, including across those lines.
bool BoundaryMasksAreContinuous()
{
    const world::PlanetDefinition planet{.radiusMeters = 6'371'000.0};
    constexpr f64 step = 0.25;
    constexpr f64 maximumJump = 0.25;
    bool ok = true;
    for (const u64 seed : {4242ULL, 7ULL, 99ULL, 123456ULL})
    {
        terrain::AnalyticTerrainDesc desc{.seed = seed};
        const terrain::AnalyticTerrainSource source(planet, desc);
        const auto& fields = source.GlobalFields();
        f64 worstConvergence = 0.0;
        f64 worstDivergence = 0.0;
        f64 worstTransform = 0.0;
        f64 worstThickness = 0.0;
        f64 worstAge = 0.0;
        for (f64 lat = -80.0; lat <= 80.0; lat += step)
        {
            for (f64 lon = -180.0; lon < 180.0 - step; lon += step)
            {
                const auto a = fields.SampleTectonicStructure(LatLonDirection(lat, lon));
                const auto b = fields.SampleTectonicStructure(LatLonDirection(lat, lon + step));
                worstConvergence = std::max(worstConvergence, std::abs(a.convergence - b.convergence));
                worstDivergence = std::max(worstDivergence, std::abs(a.divergence - b.divergence));
                worstTransform = std::max(worstTransform, std::abs(a.transform - b.transform));
                worstThickness = std::max(worstThickness, std::abs(a.crustThicknessKm - b.crustThicknessKm));
                worstAge = std::max(worstAge, std::abs(a.crustAge - b.crustAge));
            }
        }
        ok &= Check(worstConvergence <= maximumJump, "convergence must not jump across a runner-up plate change");
        ok &= Check(worstDivergence <= maximumJump, "divergence must not jump across a runner-up plate change");
        ok &= Check(worstTransform <= maximumJump, "transform must not jump across a runner-up plate change");
        ok &= Check(worstThickness <= 8.0, "crust thickness must not jump across a runner-up plate change");
        ok &= Check(worstAge <= maximumJump, "crust age must not jump across a runner-up plate change");
        if (!ok)
        {
            std::cerr << "seed " << seed << " worst jumps: convergence " << worstConvergence
                      << " divergence " << worstDivergence << " transform " << worstTransform
                      << " thickness " << worstThickness << " age " << worstAge << '\n';
            return false;
        }
    }
    return ok;
}
} // namespace

bool SubductionHasPolarity()
{
    // In an ocean-continent collision the trench lies on the oceanic
    // (descending) plate and the volcanic arc inland on the continental
    // (overriding) one. The old symmetric mask put both on both sides.
    const world::PlanetDefinition planet{.radiusMeters = 6'000'000.0};
    terrain::AnalyticTerrainDesc desc{.seed = 4242};
    const terrain::AnalyticTerrainSource source(planet, desc);
    u32 trench = 0, trenchOnOcean = 0, arc = 0, arcOnContinent = 0, anyArc = 0;
    for (u32 i = 0; i < 60'000; ++i)
    {
        const auto s = source.GlobalFields().SampleTectonicStructure(Fibonacci(i, 60'000));
        if (s.subductionTrench < 0.0 || s.subductionTrench > 1.0 ||
            s.volcanicArc < 0.0 || s.volcanicArc > 1.0)
            return Check(false, "subduction fields within 0..1");
        if (s.continental == s.neighbourContinental)
            continue;
        if (s.subductionTrench > 0.5)
        {
            ++trench;
            trenchOnOcean += s.continental ? 0U : 1U;
        }
        if (s.volcanicArc > 0.5)
        {
            ++arc;
            arcOnContinent += s.continental ? 1U : 0U;
        }
        anyArc += s.volcanicArc > 0.0 ? 1U : 0U;
    }
    std::cout << "trench " << trenchOnOcean << "/" << trench << " arc " << arcOnContinent << "/" << arc << '\n';
    bool ok = Check(trench > 20U && arc > 20U, "mixed boundaries produce a trench and an arc");
    ok &= Check(trenchOnOcean * 100U >= trench * 85U, "trench lies on the descending oceanic side");
    ok &= Check(arcOnContinent * 100U >= arc * 85U, "arc lies on the overriding continental side");
    return ok;
}

int main()
{
    const bool structure = StructureIsBoundedAndDeterministic();
    const bool continuous = BoundaryMasksAreContinuous();
    const bool polarity = SubductionHasPolarity();
    return structure && continuous && polarity ? 0 : 1;
}

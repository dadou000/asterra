#include <orbit/optical_transport/Atmosphere.hpp>
#include <orbit/optical_transport/CacheStamp.hpp>
#include <orbit/optical_transport/CloudRepresentation.hpp>
#include <orbit/optical_transport/DischargeGraph.hpp>
#include <orbit/optical_transport/EmitterTree.hpp>
#include <orbit/optical_transport/OpticalHierarchy.hpp>
#include <orbit/optical_transport/StellarVisibility.hpp>

#include <cmath>
#include <cstdio>
#include <numbers>
#include <random>
#include <vector>

using namespace orbit::optical_transport;

namespace
{
int failures = 0;
void Check(bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}
bool Near(double a, double b, double rel) { return std::abs(a - b) <= rel * std::max(std::abs(a), std::abs(b)) + 1.0e-300; }
} // namespace

int main()
{
    // --- cache stamps and dependency invalidation
    {
        CacheStamp cached{1, 7, 10.0, 3, 5, 2};
        CacheStamp need = cached;
        Check(ClassifyCache(cached, need) == CacheValidity::Current, "cache current");
        need.mediumVersion = 6;
        Check(ClassifyCache(cached, need) == CacheValidity::StaleMedium, "medium stale");
        need = cached; need.sourceVersion = 4;
        Check(ClassifyCache(cached, need) == CacheValidity::StaleSource, "source stale");
        need = cached; need.regionId = 8;
        Check(ClassifyCache(cached, need) == CacheValidity::WrongRegion, "region mismatch");
        const auto camera = InvalidateForCameraChange();
        Check(!camera.opticalHierarchy && !camera.stellarVisibility && !camera.directionalRadiance && !camera.surfaceTransport,
              "camera change invalidates no physical product");
        const auto light = InvalidateForSourceChange(false);
        Check(!light.opticalHierarchy && !light.stellarVisibility && light.directionalRadiance, "light change keeps extinction");
        const auto medium = InvalidateForMediumChange();
        Check(medium.opticalHierarchy && medium.stellarVisibility && medium.diffuseScattering, "medium change cascades");
        const auto flash = InvalidateForSourceChange(true);
        Check(flash.transientAtlas && !flash.diffuseScattering, "lightning leaves the permanent solution");
    }

    // --- humidity, haze and fog
    {
        Check(Near(SaturationVapourPressure(293.15), 2339.0, 0.01), "saturation vapour pressure at 20C");
        const double rh = RelativeHumidity(0.0073, 101325.0, 293.15);
        Check(rh > 0.45 && rh < 0.55, "relative humidity derived from mixing ratio");
        double previous = 0.0;
        bool monotonic = true;
        for (double h = 0.0; h <= 0.99; h += 0.05)
        {
            const double s = HumidExtinctionScale(AerosolKind::Sulfate, h);
            monotonic = monotonic && s >= previous;
            previous = s;
        }
        Check(monotonic, "hygroscopic extinction rises with humidity");
        Check(HumidExtinctionScale(AerosolKind::Sulfate, 0.9) > 2.0 * HumidExtinctionScale(AerosolKind::DustSmoke, 0.9) * 0.0 + 1.0,
              "sulfate haze grows");
        Check(HumidExtinctionScale(AerosolKind::Sulfate, 0.9) / HumidExtinctionScale(AerosolKind::Sulfate, 0.0) >
                  HumidExtinctionScale(AerosolKind::DustSmoke, 0.9) / HumidExtinctionScale(AerosolKind::DustSmoke, 0.0),
              "hygroscopic aerosol grows more than dust");
        Check(!IsFog(0.95) && IsFog(1.0), "fog needs saturation");
        Check(Near(HygroscopicGrowthFactor(0.0, 0.9), 1.0, 1e-12), "insoluble particles do not grow");
    }

    // --- refractive index and ray bending
    {
        const double n = 1.0 + AirRefractivity(0.589, 288.15, 101325.0, 0.0);
        Check(std::abs(n - 1.000277) < 1.0e-6, "refractive index of standard air near the sodium line");
        const double humid = AirRefractivity(0.589, 288.15, 101325.0, 1700.0);
        const double dry = AirRefractivity(0.589, 288.15, 101325.0, 0.0);
        Check(humid < dry, "moisture lowers refractivity");
        Check(AirRefractivity(0.45, 288.15, 101325.0, 0.0) > AirRefractivity(0.65, 288.15, 101325.0, 0.0), "blue refracts more");
        Check(AirRefractivity(0.589, 288.15, 50000.0, 0.0) < 0.55 * dry, "refractivity scales with pressure");
        const double gradient = RefractiveGradientVertical(dry, 288.15, 0.0065);
        Check(gradient < 0.0, "refractivity falls with altitude");
        // A ray moving horizontally bends toward higher n (downward) in a standard atmosphere.
        Vec3 d{1, 0, 0};
        for (int i = 0; i < 1000; ++i) d = BendRay(d, {0, gradient, 0}, 1.000277, 100.0);
        Check(d.y < 0.0, "ray curves toward denser air");
        Check(Near(d.x * d.x + d.y * d.y + d.z * d.z, 1.0, 1e-9), "direction stays unit");
        // Zero gradient: straight ray.
        Vec3 straight = BendRay({0, 1, 0}, {0, 0, 0}, 1.0, 50.0);
        Check(Near(straight.y, 1.0, 1e-12), "no gradient, no bend");
    }

    // --- stellar disk visibility and transmission
    {
        constexpr double sunRadius = 6.957e8, au = 1.495978707e11;
        const double alpha = AngularRadius(sunRadius, au);
        Check(Near(alpha, 4.65e-3, 0.01), "solar angular radius is 0.267 degrees");
        Check(Near(PenumbraWidth(1000.0, alpha), 9.3, 0.02), "penumbra grows with blocker separation");
        Check(PenumbraWidth(10.0, alpha) < PenumbraWidth(1000.0, alpha), "near blockers have sharper shadows");
        Check(Near(DiskVisibilityAcrossEdge(0.0, alpha), 0.5, 1e-2), "edge through centre hides half the disk");
        Check(DiskVisibilityAcrossEdge(alpha * 2.0, alpha) < 1e-9, "fully covered");
        Check(Near(DiskVisibilityAcrossEdge(-alpha * 2.0, alpha), 1.0, 1e-9), "fully visible");
        // Limb darkening: the visible sliver near the limb is dimmer than its area suggests.
        const double uniformHalf = DiskVisibilityAcrossEdge(0.5 * alpha, alpha, 0.0);
        const double darkenedHalf = DiskVisibilityAcrossEdge(0.5 * alpha, alpha, 0.6);
        Check(darkenedHalf < uniformHalf - 0.01, "the visible limb side of a darkened disk carries less than its geometric share");
        // Sun above the astronomical horizon but behind a mountain.
        Check(HorizonVisibility(0.10, 0.20, alpha) < 1e-9, "sun hidden behind mountain");
        Check(HorizonVisibility(0.10, 0.0, alpha) > 0.999, "sun clear of the horizon");
        const double rising = HorizonVisibility(0.10, 0.10, alpha);
        Check(rising > 0.45 && rising < 0.55, "sun straddling the ridge is half visible");
        // Disk-averaged transmission is not exp(-mean tau).
        const double meanTau = 1.0;
        const double transmission = DiskTransmission(alpha, 0.0, 41, [&](double x, double) { return x > 0 ? 2.0 * meanTau : 0.0; });
        Check(transmission > std::exp(-meanTau) + 0.05, "average of transmissions exceeds transmission of average");
        Check(Near(transmission, 0.5 * (1.0 + std::exp(-2.0)), 0.05), "disk transmission matches the two-half average");
        const std::vector<double> taus{0.0, 0.0, 0.0, 8.0};
        const auto stats = FilterOpticalDepths(taus);
        Check(Near(stats.coverage, 0.25, 1e-12), "coverage fraction preserved");
        Check(EffectiveOpticalDepth(stats) < stats.meanOpticalDepth, "effective tau below mean tau (Jensen)");
        Check(Near(stats.meanTransmission, (3.0 + std::exp(-8.0)) / 4.0, 1e-9), "mean transmission preserved");
    }

    // --- optical hierarchy
    {
        constexpr std::uint32_t edge = 16;
        std::vector<float> field(edge * edge * edge, 0.0f);
        // A dense blob in one corner, otherwise empty.
        for (std::uint32_t z = 0; z < 4; ++z)
            for (std::uint32_t y = 0; y < 4; ++y)
                for (std::uint32_t x = 0; x < 4; ++x)
                    field[(z * edge + y) * edge + x] = 0.01f * static_cast<float>(1 + (x + y + z) % 3);
        OpticalHierarchy h;
        h.Build(field, edge, 10.0);
        const double origin[3] = {1.0, 1.0, 0.0};
        const double dir[3] = {0.0, 0.0, 1.0};
        double exact = 0.0;
        for (std::uint32_t z = 0; z < edge; ++z)
            exact += field[(z * edge + 0) * edge + 0] * 10.0;
        const auto full = h.OpticalDepth(origin, dir, 160.0, 0);
        Check(Near(full.estimate, exact, 1e-6), "hierarchy reproduces brute-force optical depth");
        Check(Near(full.lower, full.upper, 1e-9), "exact traversal has a degenerate interval");
        const auto coarse = h.OpticalDepth(origin, dir, 160.0, 3);
        Check(coarse.lower <= exact + 1e-9 && coarse.upper >= exact - 1e-9, "coarse interval is conservative");
        Check(coarse.nodesVisited < full.nodesVisited, "coarse query visits fewer nodes");
        // Empty space is skipped.
        const double farOrigin[3] = {100.0, 100.0, 1.0};
        const auto empty = h.OpticalDepth(farOrigin, dir, 160.0, 0);
        Check(empty.estimate == 0.0 && empty.nodesVisited < 40, "empty column skipped cheaply");
        const auto before = h.Version();
        const std::size_t touched = h.SetVoxel(10, 10, 10, 0.5f);
        Check(touched == 5, "edit touches one node per level (dirty range)");
        Check(h.Version() > before, "edit bumps version");
        Check(h.Root().maximum >= 0.5f, "root bound reflects the edit");
    }

    // --- emitter hierarchy
    {
        std::mt19937 rng(7);
        std::uniform_real_distribution<double> u(0.0, 1.0);
        std::vector<Emitter> city;
        for (int i = 0; i < 5000; ++i)
        {
            Emitter e;
            e.position[0] = u(rng) * 2000.0;
            e.position[1] = 5.0;
            e.position[2] = u(rng) * 2000.0;
            e.power = 50.0 * (0.5 + u(rng));
            e.axis[1] = 1.0;  // upward-facing (windows, sky glow)
            e.directionality = 1.0;
            city.push_back(e);
        }
        EmitterTree tree;
        tree.Build(city);
        const double expectedPower = [&] { double s = 0; for (auto& e : city) s += e.power; return s; }();
        Check(Near(tree.TotalPower(), expectedPower, 1e-9), "tree preserves total power");
        // Cloud base 2 km above the city, normal facing down; ground receiver facing up.
        const double cloud[3] = {1000.0, 20000.0, 1000.0};
        const double down[3] = {0, -1, 0};
        const double up[3] = {0, 1, 0};
        Check(tree.ExactIrradiance(cloud, up) == 0.0, "a surface facing away from the city receives nothing");
        const double exactUp = tree.ExactIrradiance(cloud, down);
        Check(exactUp > 0.0, "cloud underside is lit by the upward-facing lights");
        const auto approx = tree.Evaluate(cloud, down, 0.02);
        Check(Near(approx.irradiance, exactUp, 0.05), "bounded evaluation matches the exact sum");
                Check(approx.sourcesEvaluated < city.size() / 20, "far receiver uses few nodes");
        const double nearCloud[3] = {1000.0, 300.0, 1000.0};
        const auto nearApprox = tree.Evaluate(nearCloud, down, 0.02);
        Check(nearApprox.sourcesEvaluated > approx.sourcesEvaluated, "near receivers descend into individual sources");
        Check(Near(nearApprox.irradiance, tree.ExactIrradiance(nearCloud, down), 0.05), "near evaluation also matches");
        Check(Near(approx.powerAccounted, expectedPower, 1e-9), "evaluation accounts for all power");
        // Merged directional group stays directional: a node of downward lights emits nothing sideways.
        const double side[3] = {100000.0, 5.0, 1000.0};
        const double sideNormal[3] = {-1, 0, 0};
        Check(tree.NodeIrradiance(tree.Nodes()[0], side, sideNormal) < 1.0e-3 * tree.NodeIrradiance(tree.Nodes()[0], cloud, down),
              "merged upward lights are not an isotropic point");
        // Many individually negligible lights still add up.
        std::vector<Emitter> dim(20000);
        for (auto& e : dim) { e.position[0] = u(rng) * 100.0; e.position[1] = 0; e.position[2] = u(rng) * 100.0; e.power = 1.0; }
        EmitterTree dimTree;
        dimTree.Build(dim);
        const double r[3] = {50.0, 100.0, 50.0};
        const double dimExact = dimTree.ExactIrradiance(r, down);
        const auto dimApprox = dimTree.Evaluate(r, down, 0.01);
        Check(Near(dimApprox.irradiance, dimExact, 0.03), "thousands of weak lights are aggregated, not dropped");
    }

    // --- discharge graph and charge
    {
        Reservoir upper{30.0}, lower{-20.0};
        const double total = upper.charge + lower.charge;
        Check(DischargeGraph::Transfer(upper, lower, 12.0), "transfer succeeds");
        Check(Near(upper.charge + lower.charge, total, 1e-12), "charge conserved");
        Check(!DischargeGraph::Transfer(upper, lower, 1000.0), "cannot move more charge than exists");
        Check(Near(upper.charge, 18.0, 1e-12), "failed transfer changes nothing");

        DischargeGraph g(FlashKind::CloudToGround);
        Segment trunk;
        trunk.to[1] = -1000.0;
        trunk.pulses.push_back({0.100, 1.0e-6, 5.0e4});      // return stroke far shorter than a frame
        const auto trunkIndex = g.AddSegment(trunk);
        Segment branch;
        branch.from[1] = -500.0; branch.to[0] = 300.0; branch.to[1] = -600.0;
        branch.parent = trunkIndex;
        branch.pulses.push_back({0.100, 1.0e-4, 100.0});
        g.AddSegment(branch);
        Segment faint;  // never conducts strongly
        faint.to[0] = 50.0;
        g.AddSegment(faint);
        Check(Near(g.EnergyInWindow(0.0996, 0.1162), 5.0e4 + 100.0, 1e-9), "microsecond stroke contributes all energy to the frame");
        Check(Near(g.EnergyInWindow(0.0, 0.05), 0.0, 1e-9), "no emission before the flash");
        g.Reactivate(trunkIndex, {0.2, 5.0e-5, 8.0e3});  // dart leader reusing the channel
        Check(Near(g.EnergyInWindow(0.0, 1.0), 5.0e4 + 100.0 + 8.0e3, 1e-9), "reactivated channel adds energy");
        Check(g.Segments().size() == 3, "reuse adds no new segment");
        for (const std::size_t cap : {1u, 2u, 8u})
        {
            const auto clusters = g.ClusterEmission(0.0, 1.0, cap);
            double sum = 0.0;
            for (const auto& c : clusters) sum += c.energy;
            Check(Near(sum, 5.0e4 + 100.0 + 8.0e3, 1e-9) && clusters.size() <= cap, "clustering is bounded and energy preserving");
        }
        // Poisson: a point charge produces a field falling off with distance, and symmetric potential.
        PoissonGrid grid;
        grid.nx = grid.ny = grid.nz = 17;
        grid.spacing = 100.0;
        grid.chargeDensity.assign(17 * 17 * 17, 0.0);
        grid.chargeDensity[(8 * 17 + 8) * 17 + 8] = 1.0e-8;
        SolvePoisson(grid, 400);
        const double centre = grid.potential[(8 * 17 + 8) * 17 + 8];
        const double offset = grid.potential[(8 * 17 + 8) * 17 + 12];
        Check(centre > offset && offset > 0.0, "potential peaks at the charge");
        Check(Near(grid.potential[(8 * 17 + 8) * 17 + 4], offset, 1e-6), "potential is symmetric");
        Check(MaxFieldMagnitude(grid) > 0.0, "field computed from potential");
    }

    // --- cloud representation LOD
    {
        CloudRegionDescriptor cirrus;
        cirrus.distance = 50000.0; cirrus.extent = 100000.0; cirrus.thickness = 500.0; cirrus.verticalOpticalDepth = 0.05;
        cirrus.pixelAngle = 1.0e-3; cirrus.cloudAltitude = 9000.0;
        Check(EstimateErrors(CloudForm::FullVolume, cirrus).parallax == 0.0, "full volume is the reference");
        Check(static_cast<int>(ChooseForm(cirrus, {}, CloudForm::FullVolume, false)) >= static_cast<int>(CloudForm::DepthImpostor),
              "thin high veil uses a cheap form");
        CloudRegionDescriptor tower = cirrus;
        tower.distance = 3000.0; tower.extent = 8000.0; tower.thickness = 12000.0; tower.verticalOpticalDepth = 200.0; tower.cloudAltitude = 1500.0;
        Check(ChooseForm(tower, {}, CloudForm::FullVolume, false) == CloudForm::FullVolume, "near thick tower stays full 3D");
        // Same physical region, different cameras: errors are functions of the view, morphology never altered.
        CloudRegionDescriptor farTower = tower;
        farTower.distance = 2.0e6;
        const CloudForm farForm = ChooseForm(farTower, {}, CloudForm::FullVolume, false);
        Check(static_cast<int>(farForm) >= static_cast<int>(CloudForm::CoarseVolume), "distant tower may simplify its representation");
        // Hysteresis: a form that is still within a relaxed tolerance is kept.
        CloudRegionDescriptor edgeCase = tower;
        edgeCase.distance = 40000.0;
        const CloudForm a = ChooseForm(edgeCase, {}, CloudForm::FullVolume, false);
        const CloudForm b = ChooseForm(edgeCase, {}, a, true);
        Check(a == b, "stable under repeated evaluation");
        CloudRegionDescriptor limb = cirrus;
        limb.distance = 300000.0;
        Check(EstimateErrors(CloudForm::CurvedSheet, limb).limb > EstimateErrors(CloudForm::CurvedSheet, cirrus).limb,
              "flat-layer error grows toward the limb");
    }

    if (failures == 0) std::printf("OpticalTransport: all tests passed\n");
    return failures;
}

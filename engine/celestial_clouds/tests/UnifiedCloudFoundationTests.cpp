#include <orbit/celestial_clouds/UnifiedCloudFoundation.hpp>
#include <orbit/celestial_clouds/CloudField.hpp>

// Keep regression assertions active even when CTest is built in Release.
#if defined(NDEBUG)
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <optional>
#include <stdexcept>

using namespace orbit::celestial_clouds::unified;

namespace
{
bool Near(const double a, const double b)
{
    return std::abs(a - b) < 1.0e-11;
}

void TestBackendIsolation()
{
    assert(RequireAvailableBackend(CloudBackend::BaselineA, false) == CloudBackend::BaselineA);
    bool refused = false;
    try { static_cast<void>(RequireAvailableBackend(CloudBackend::UnifiedB, false)); }
    catch (const std::logic_error&) { refused = true; }
    assert(refused);
    assert(RequireAvailableBackend(CloudBackend::UnifiedB, true) == CloudBackend::UnifiedB);
    assert(!BackendReadiness{}.FullABReady());
    assert((BackendReadiness{true, true, true, true, true}).FullABReady());
}

void TestInvalidation()
{
    FrameSignature a{};
    a.worldGeneration = 1;
    a.bodyKey = 22;
    a.width = 1280;
    a.height = 720;
    a.cloudFieldFingerprint = 1;
    a.terrainRevision = 3;
    a.atmosphereRevision = 5;
    a.sunRevision = 9;
    a.qualityRevision = 1;

    const auto initial = Invalidate(std::nullopt, a);
    assert(initial.density && initial.sunlight && initial.temporalHistory && initial.renderTargets);
    assert(!Invalidate(a, a).Any());
    auto b = a;
    b.sunRevision++;
    const auto movingSun = Invalidate(a, b);
    assert(movingSun.sunlight && movingSun.receiverShadows && movingSun.aerialPerspective);
    assert(!movingSun.density && !movingSun.temporalHistory && !movingSun.renderTargets);
    b = a;
    b.cloudFieldFingerprint++;
    const auto weather = Invalidate(a, b);
    assert(weather.density && weather.sunlight && weather.receiverShadows);
    assert(!weather.temporalHistory && !weather.renderTargets);
    b = a;
    b.stormRevision++;
    assert(Invalidate(a, b).density);
    b = a;
    b.terrainRevision++;
    const auto editedTerrain = Invalidate(a, b);
    assert(editedTerrain.sunlight && editedTerrain.temporalHistory && editedTerrain.receiverShadows);
    assert(!editedTerrain.density);
    b = a;
    b.atmosphereRevision++;
    const auto changedAir = Invalidate(a, b);
    assert(changedAir.aerialPerspective && changedAir.sunlight && changedAir.temporalHistory);
    assert(!changedAir.density);
    b = a;
    b.width++;
    const auto resized = Invalidate(a, b);
    assert(resized.renderTargets && resized.temporalHistory && !resized.density);
    b = a;
    b.worldGeneration++;
    assert(Invalidate(a, b).density && Invalidate(a, b).renderTargets);
    b = a;
    b.bodyKey++;
    assert(Invalidate(a, b).density);
    b = a;
    b.discontinuity = true;
    assert(Invalidate(a, b).temporalHistory && Invalidate(a, b).sunlight);
    b = a;
    b.qualityRevision++;
    assert(Invalidate(a, b).renderTargets && Invalidate(a, b).temporalHistory);
    b = a;
    b.width = 0;
    bool rejected = false;
    try { static_cast<void>(Invalidate(a, b)); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
}

void TestLayerPackingAndOverlap()
{
    using namespace orbit::celestial_clouds;
    static_assert(sizeof(GpuCloudTexel) == 16U);
    static_assert(sizeof(GpuCloudLayerDescriptor) == 32U);

    CloudLayerParameters low{};
    low.baseAltitudeMeters = 500.0;
    low.topAltitudeMeters = 1800.0;
    low.peakOpticalDepth = 6.0;
    low.coverageBias = 0.1;
    low.densityExponent = 1.4;
    low.singleScatteringAlbedo = 0.95;
    low.anisotropy = 0.6;
    low.shadowParticipation = false;
    auto high = low;
    high.baseAltitudeMeters = 1400.0;
    high.topAltitudeMeters = 8500.0;
    high.peakOpticalDepth = 12.0;
    high.shadowParticipation = true;

    const auto a = EncodeGpuCloudLayer(low);
    const auto b = EncodeGpuCloudLayer(high);
    assert(Near(a.shell[0], 500.0) && Near(a.shell[1], 1800.0));
    assert(Near(a.shell[2], 6.0) && Near(a.shell[3], 0.1));
    assert(Near(a.material[0], 1.4));
    assert(Near(a.material[1], 0.95));
    assert(Near(a.material[2], 0.6));
    assert(Near(a.material[3], 0.0));
    assert(Near(b.shell[0], 1400.0) && Near(b.shell[1], 8500.0));
    assert(Near(b.material[3], 1.0));

    // GPU layout is six cube faces per layer, 16 bytes per texel,
    // followed by 32 bytes per layer descriptor. A's texel offsets
    // never change when B descriptors are appended.
    constexpr std::size_t resolution = 193;
    constexpr std::size_t layers = 2;
    constexpr std::size_t texelBytes =
        resolution * resolution * 6U * layers * sizeof(GpuCloudTexel);
    constexpr std::size_t descriptorBytes =
        layers * sizeof(GpuCloudLayerDescriptor);
    assert(texelBytes % 16U == 0U && descriptorBytes == 64U);

    // Two overlapping media add extinction before Beer attenuation.
    // Replacing one layer or double-compositing destroys this identity.
    const double lowSigma = 0.004, highSigma = 0.002;
    const double segment = 400.0;
    assert(Near(std::exp(-(lowSigma+highSigma)*segment),
        std::exp(-lowSigma*segment)*std::exp(-highSigma*segment)));

    // Receiver-shadow participation is independently authored per layer.
    const double groundTau = lowSigma*a.material[3] +
        highSigma*b.material[3];
    assert(Near(groundTau, highSigma));
}

void TestMultiViewIsolation()
{
    ViewRegistry views;
    FrameSignature frame{};
    frame.bodyKey = 42U;
    frame.worldGeneration = 1U;
    frame.width = 1024U;
    frame.height = 768U;

    assert(views.Observe("studio.primary", frame).density);
    assert(views.Observe("studio.secondary", frame).density);
    assert(!views.Observe("studio.primary", frame).Any());

    FrameSignature changed = frame;
    changed.sunRevision++;
    assert(views.Observe("studio.secondary", changed).sunlight);
    assert(!views.Observe("studio.primary", frame).Any());
    views.Forget("studio.primary");
    assert(views.Size() == 1U);
    assert(views.Observe("studio.primary", frame).renderTargets);
    views.Reset();
    assert(views.Size() == 0U);

    bool refused = false;
    try { static_cast<void>(views.Observe("", frame)); }
    catch (const std::invalid_argument&) { refused = true; }
    assert(refused);
    assert(ResourceBudget{}.Valid());
    assert(ResourceBudget{}.FitsPreviewTarget(1920U, 1080U));
    assert(!ResourceBudget{}.FitsPreviewTarget(16384U, 16384U));
    assert(!ResourceBudget{}.FitsPreviewTarget(0U, 1080U));
    assert(!ResourceBudget{}.FitsPreviewTarget(1080U, 0U));
    ResourceBudget invalidBudget{};
    invalidBudget.maximumUpdatesPerFrame = 0U;
    assert(!invalidBudget.Valid());
}

void TestRadianceComposition()
{
    const IntegratedMedium clear{};
    const IntegratedMedium front{{3.0, 1.0, 0.5}, 0.4};
    const IntegratedMedium behind{{1.0, 2.0, 3.0}, 0.2};
    const auto result = CompositeFrontToBack(front, behind);
    assert(Near(result.transmittance, 0.08));
    assert(Near(result.radiance[0], 3.4));
    assert(Near(result.radiance[1], 1.8));
    assert(Near(result.radiance[2], 1.7));
    assert(Near(CompositeFrontToBack(clear, front).radiance[0], front.radiance[0]));
    assert(Near(CompositeFrontToBack(front, clear).transmittance, front.transmittance));

    const IntegratedMedium third{{0.4, 0.5, 0.6}, 0.3};
    const auto left = CompositeFrontToBack(CompositeFrontToBack(front, behind), third);
    const auto right = CompositeFrontToBack(front, CompositeFrontToBack(behind, third));
    assert(Near(left.transmittance, right.transmittance));
    for (std::size_t i = 0; i < 3; ++i)
        assert(Near(left.radiance[i], right.radiance[i]));

    bool rejected = false;
    try { static_cast<void>(CompositeFrontToBack({{-1., 0., 0.}, 1.}, front)); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
}
}

int main()
{
    TestBackendIsolation();
    TestInvalidation();
    TestRadianceComposition();
    TestMultiViewIsolation();
    TestLayerPackingAndOverlap();
}

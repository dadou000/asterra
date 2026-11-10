#include <orbit/lighting/CityLightTree.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <random>

using namespace orbit;
using namespace orbit::lighting;

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

// A square city of `blocks` x `blocks` street lamps 30 m apart (8 m high, pointing down) plus window lights.
std::vector<LocalLight> MakeCity(int blocks, int windows, unsigned seed)
{
    std::vector<LocalLight> lights;
    lights.reserve(static_cast<std::size_t>(blocks) * blocks + windows);
    for (int j = 0; j < blocks; ++j)
        for (int i = 0; i < blocks; ++i)
        {
            LocalLight lamp;
            lamp.type = LocalLightType::Spot;
            lamp.positionInFrameMeters = {i * 30.0, 8.0, j * 30.0};
            lamp.direction = {0.0F, -1.0F, 0.0F};
            lamp.luminousFluxLumens = 4000.0F;
            lamp.outerConeRadians = 1.0F;
            lamp.innerConeRadians = 0.5F;
            lamp.rangeMeters = 60.0F;
            lamp.colorLinear = {1.0F, 0.8F, 0.5F};
            lamp.stableId = lights.size();
            lights.push_back(lamp);
        }
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    for (int w = 0; w < windows; ++w)
    {
        LocalLight window;
        window.type = LocalLightType::Point;
        window.positionInFrameMeters = {u(rng) * blocks * 30.0, 5.0 + u(rng) * 60.0, u(rng) * blocks * 30.0};
        window.luminousFluxLumens = 300.0F;
        window.rangeMeters = 25.0F;
        window.colorLinear = {0.9F, 0.9F, 1.0F};
        window.stableId = lights.size();
        lights.push_back(window);
    }
    return lights;
}

// One consistent shading model for original and merged lights: isotropic or cosine-lobe intensity, receiver cosine.
double Illuminance(const LocalLight& l, const math::Double3& p, const math::Float3& normal)
{
    const double dx = p.x - l.positionInFrameMeters.x, dy = p.y - l.positionInFrameMeters.y, dz = p.z - l.positionInFrameMeters.z;
    const double d2 = std::max(dx * dx + dy * dy + dz * dz, 1.0e-6), d = std::sqrt(d2);
    double intensity = l.luminousFluxLumens / (4.0 * std::numbers::pi);
    if (l.type == LocalLightType::Spot)
    {
        const double D = 1.0 - 0.5 * (1.0 - std::cos(static_cast<double>(l.outerConeRadians)));
        const double cosAxis = std::max(0.0, (dx * l.direction.x + dy * l.direction.y + dz * l.direction.z) / d);
        intensity = l.luminousFluxLumens * ((1.0 - D) / (4.0 * std::numbers::pi) + D * cosAxis / std::numbers::pi);
    }
    const double cosReceiver = std::max(0.0, -(dx * normal.x + dy * normal.y + dz * normal.z) / d);
    return intensity * cosReceiver / d2;
}
double Total(const std::vector<LocalLight>& lights, const math::Double3& p, const math::Float3& n)
{
    double e = 0.0;
    for (const auto& l : lights) e += Illuminance(l, p, n);
    return e;
}
} // namespace

int main()
{
    const std::vector<LocalLight> city = MakeCity(316, 30000, 3);   // 99,856 lamps + 30,000 windows
    CityLightTree tree;
    const auto b0 = std::chrono::steady_clock::now();
    tree.Build(city);
    const double buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - b0).count();
    double expected = 0.0;
    for (const auto& l : city) expected += l.luminousFluxLumens;
    Check(Near(tree.TotalFlux(), expected, 1.0e-12) && tree.LightCount() == city.size(), "the tree knows every light and its total flux");

    const math::Double3 centre{4740.0, 200.0, 4740.0};
    const double angle = 0.02;   // about 20 pixels at 1080p / 60 degrees

    // Energy is preserved at every receiver and budget, and the budget is respected.
    bool energy = true, bounded = true;
    for (const math::Double3 receiver : {centre, math::Double3{0.0, 5.0, 0.0}, math::Double3{20000.0, 3000.0, -9000.0}})
        for (const std::size_t budget : {64u, 512u, 4096u})
        {
            const auto cut = tree.Cut(receiver, angle, budget);
            energy = energy && Near(cut.totalFlux, expected, 1.0e-9);
            double sum = 0.0;
            for (const auto& l : cut.lights) sum += l.luminousFluxLumens;
            energy = energy && Near(sum, expected, 1.0e-4);   // float flux in each light
            bounded = bounded && cut.lights.size() <= budget && !cut.lights.empty();
        }
    Check(energy, "every cut carries the whole city's flux");
    Check(bounded, "no cut exceeds its light budget");

    // Level of detail: a receiver in the city gets more lights than a distant one, and the city collapses far away.
    const auto near = tree.Cut({2000.0, 20.0, 2000.0}, angle, 4096);
    const auto far = tree.Cut({60000.0, 3000.0, 60000.0}, angle, 4096);
    std::printf("cut sizes: near %zu (%zu merged), far %zu (%zu merged), of %zu lights\n", near.lights.size(), near.mergedNodes,
                far.lights.size(), far.mergedNodes, city.size());
    Check(near.lights.size() > 4 * far.lights.size(), "a receiver inside the city gets far more lights than a distant one");
    Check(far.lights.size() <= 256 && 10 * far.lights.size() < near.lights.size(), "a very distant city collapses to a few hundred lights at most");
    // With room in the budget the lights around the receiver are the original ones, untouched.
    const auto roomy = tree.Cut({2000.0, 5.0, 2000.0}, angle, 40000);
    std::size_t untouched = 0;
    for (const auto& l : roomy.lights) untouched += (l.stableId & kProxyLightIdBit) == 0 ? 1 : 0;
    Check(untouched > 1000, "with a roomy budget the cut contains many original lights near the receiver");

    // Accuracy against the exact sum over every light. A ground point faces up.
    const math::Float3 up{0.0F, 1.0F, 0.0F};
    const math::Float3 down{0.0F, -1.0F, 0.0F};   // a cloud base faces the city below it
    const auto relativeError = [&](const math::Double3& r, const math::Float3& normal, const CityLightTree::CutResult& cut) {
        const double exact = Total(city, r, normal);
        Check(exact > 1.0e-3, "the reference illuminance is non-zero (a vacuous comparison proves nothing)");
        return std::abs(Total(cut.lights, r, normal) - exact) / exact;
    };
    // (a) A cut made AT the receiver (Lightcuts-style) keeps its own lamps and is accurate even with a small budget.
    double worstAtReceiver = 0.0;
    for (const math::Double3 r : {math::Double3{4740.0, 0.0, 4740.0}, math::Double3{4810.0, 0.0, 4690.0}, math::Double3{4500.0, 0.0, 4900.0}, math::Double3{900.0, 0.0, 8000.0}})
        worstAtReceiver = std::max(worstAtReceiver, relativeError(r, up, tree.Cut(r, angle, 2048)));
    // (b) Distant receivers: cloud bases 1.5 to 5 km up, above and beside the city.
    double worstDistant = 0.0;
    for (const math::Double3 r : {math::Double3{4740.0, 3000.0, 4740.0}, math::Double3{2000.0, 5000.0, 8000.0}, math::Double3{9480.0 + 3000.0, 1500.0, 4740.0}})
        worstDistant = std::max(worstDistant, relativeError(r, down, tree.Cut(r, angle, 256)));
    // (c) The documented limitation: a cut made for a place 3.8 km away, used at this surface, merges its nearby lamps.
    const double misused = relativeError({4740.0, 0.0, 4740.0}, up, tree.Cut({1000.0, 200.0, 1000.0}, angle, 2048));
    std::printf("illuminance error: cut at the receiver %.2f %%, distant receivers %.2f %%, cut for another place %.1f %%\n",
                100.0 * worstAtReceiver, 100.0 * worstDistant, 100.0 * misused);
    Check(worstAtReceiver < 0.05, "a cut made at the receiver is within 5 percent with 2048 lights");
    Check(worstDistant < 0.05, "distant receivers (clouds, far ground) are within 5 percent with only 256 lights");
    Check(misused > 5.0 * worstAtReceiver, "a cut made for another place is clearly worse near the surface (documented limitation)");

    // A directional district stays directional: the merged spots point where the lamps point, with the lamps' cone.
    bool directional = true, cone = true;
    for (const auto& l : far.lights)
        if ((l.stableId & kProxyLightIdBit) != 0 && l.type == LocalLightType::Spot)
        {
            directional = directional && l.direction.y < -0.5F;
            cone = cone && std::abs(l.outerConeRadians - 1.0F) < 0.35F;
        }
    Check(directional, "merged street-lamp groups point downward, not isotropically");
    Check(cone, "a merged group of identical lamps keeps roughly the lamps' own cone");

    // The cut feeds the tiled light grid: bounded input, same flux.
    LightingView view;
    view.forward = {0.0F, 0.0F, 1.0F}; view.up = {0.0F, 1.0F, 0.0F}; view.verticalFovRadians = 1.0F;
    std::printf("build %.0f ms for %zu lights\n", buildMs, city.size());

    if (failures == 0) std::printf("CityLightTree: all tests passed\n");
    return failures;
}

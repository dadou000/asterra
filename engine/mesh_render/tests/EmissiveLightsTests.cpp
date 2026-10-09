#include <orbit/mesh_render/EmissiveLights.hpp>

#include <cmath>
#include <numbers>
#include <vector>

namespace
{
int failures = 0;

void Check(const bool condition)
{
    if (!condition)
    {
        ++failures;
    }
}

bool Near(const float a, const float b, const float relative = 1.0e-3F)
{
    return std::abs(a - b) <= relative * std::max(std::abs(b), 1.0e-6F);
}
} // namespace

int main()
{
    using namespace orbit::mesh_render;
    constexpr float pi = std::numbers::pi_v<float>;

    // Analytic areas.
    Check(Near(EmissiveSurfaceArea(0U, {0.5F, 0.5F, 0.5F}), 6.0F));      // unit cube
    Check(Near(EmissiveSurfaceArea(1U, {1.0F, 1.0F, 1.0F}), 4.0F * pi)); // unit sphere
    // Ellipsoid approximation stays within 1.1 % of an oblate spheroid's area.
    {
        const float a = 2.0F;
        const float c = 1.0F;
        const float e = std::sqrt(1.0F - c * c / (a * a));
        const float exact = 2.0F * pi * a * a +
                            pi * c * c / e * std::log((1.0F + e) / (1.0F - e));
        Check(Near(EmissiveSurfaceArea(1U, {a, a, c}), exact, 0.011F));
    }
    Check(Near(EmissiveSurfaceArea(2U, {1.0F, 2.0F, 1.0F}), 2.0F * pi * 1.0F * 4.0F + 2.0F * pi, 1.0e-3F));
    // Capsule: a sphere of radius r plus a cylinder shaft of length 2s.
    Check(Near(EmissiveSurfaceArea(3U, {0.5F, 1.5F, 0.5F}),
               4.0F * pi * 0.25F + 2.0F * pi * 0.5F * 2.0F));
    // A capsule shorter than its diameter is a sphere.
    Check(Near(EmissiveSurfaceArea(3U, {1.0F, 0.2F, 1.0F}), 4.0F * pi));
    // A plane emits from both faces.
    Check(Near(EmissiveSurfaceArea(4U, {1.0F, 0.01F, 2.0F}), 16.0F));
    // Unknown shapes and degenerate extents emit nothing.
    Check(EmissiveSurfaceArea(9U, {1.0F, 1.0F, 1.0F}) == 0.0F);
    Check(EmissiveSurfaceArea(0U, {1.0F, 0.0F, 1.0F}) == 0.0F);

    // The equivalent sphere has the same area.
    Check(Near(EquivalentSphereRadius(4.0F * pi * 0.09F), 0.3F));
    Check(EquivalentSphereRadius(0.0F) == 0.0F);
    Check(EquivalentSphereRadius(-1.0F) == 0.0F);

    // Selection keeps the strongest lights at the camera and drops dark ones.
    {
        std::vector<EmissiveLight> lights;
        for (int i = 0; i < 20; ++i)
        {
            EmissiveLight light;
            light.position = {static_cast<float>(i + 1), 0.0F, 0.0F};
            light.radius = 0.5F;
            light.radiance = {1.0F, 1.0F, 1.0F};
            lights.push_back(light);
        }
        EmissiveLight dark;
        dark.position = {0.1F, 0.0F, 0.0F};
        dark.radiance = {0.0F, 0.0F, 0.0F};
        lights.push_back(dark);

        unsigned count = 0U;
        const auto selected = SelectEmissiveLights(lights, count);
        Check(count == kMaxEmissiveLights);
        // Equal power: the nearest ones win, in order.
        Check(Near(selected[0].position[0], 1.0F));
        Check(Near(selected[kMaxEmissiveLights - 1U].position[0],
                   static_cast<float>(kMaxEmissiveLights)));
        for (unsigned i = 0U; i < count; ++i)
        {
            Check(selected[i].radiance[0] > 0.0F);
        }
    }

    // A brighter, bigger light beats a nearer dim one.
    {
        EmissiveLight dim;
        dim.position = {2.0F, 0.0F, 0.0F};
        dim.radius = 0.1F;
        dim.radiance = {0.01F, 0.01F, 0.01F};
        EmissiveLight bright;
        bright.position = {6.0F, 0.0F, 0.0F};
        bright.radius = 1.0F;
        bright.radiance = {1.0F, 1.0F, 1.0F};
        const std::vector<EmissiveLight> lights{dim, bright};
        unsigned count = 0U;
        const auto selected = SelectEmissiveLights(lights, count);
        Check(count == 2U);
        Check(Near(selected[0].position[0], 6.0F));
    }

    // No lights: empty selection.
    {
        unsigned count = 7U;
        static_cast<void>(SelectEmissiveLights({}, count));
        Check(count == 0U);
    }

    return failures;
}

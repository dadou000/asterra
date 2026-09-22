#include <orbit/volume_render/UniversalVolumeRenderer.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <source_location>

namespace
{
void Check(
    const bool condition,
    const std::source_location location =
        std::source_location::current())
{
    if (!condition)
    {
        std::cerr
            << "Universal volume renderer test failed at "
            << location.file_name()
            << ':'
            << location.line()
            << '\n';
        std::exit(1);
    }
}

bool Near(
    const float a,
    const float b,
    const float epsilon = 1.0e-5F)
{
    return
        std::abs(a - b) <=
        epsilon;
}
} // namespace

int main()
{
    using namespace orbit;
    using namespace orbit::volume_render;

    {
        const auto result =
            IntegrateHomogeneousVolume(
                1.0F,
                2.0F,
                0.5F,
                0.0F,
                {1.0F,1.0F,1.0F},
                {1.0F,1.0F,1.0F},
                {0.0F,0.0F,0.0F},
                0.0F,
                0.0F);

        Check(
            Near(
                result.transmittance,
                std::exp(-1.0F)));
        Check(
            Near(
                result.totalRadiance.x,
                0.0F));
    }

    {
        const auto result =
            IntegrateHomogeneousVolume(
                1.0F,
                1.0F,
                1.0F,
                1.0F,
                {1.0F,0.5F,0.25F},
                {2.0F,2.0F,2.0F},
                {0.0F,0.0F,0.0F},
                0.0F,
                0.0F);

        const float integral =
            1.0F -
            std::exp(-1.0F);

        Check(
            Near(
                result.scatteredRadiance.x,
                2.0F * integral));
        Check(
            Near(
                result.scatteredRadiance.y,
                1.0F * integral));
        Check(
            Near(
                result.scatteredRadiance.z,
                0.5F * integral));
    }

    {
        const auto result =
            IntegrateHomogeneousVolume(
                2.0F,
                0.5F,
                1.0F,
                0.0F,
                {1.0F,1.0F,1.0F},
                {0.0F,0.0F,0.0F},
                {4.0F,2.0F,1.0F},
                3.0F,
                0.5F);

        Check(
            result.emittedRadiance.x >
            result.emittedRadiance.y);
        Check(
            result.emittedRadiance.y >
            result.emittedRadiance.z);
        Check(
            Near(
                result.totalRadiance.x,
                result.emittedRadiance.x));
    }

    {
        const float isotropic =
            HenyeyGreensteinPhase(
                0.0F,
                0.0F);

        Check(
            Near(
                isotropic,
                1.0F /
                    (4.0F *
                     3.14159265358979323846F),
                1.0e-6F));

        const float forward =
            HenyeyGreensteinPhase(
                1.0F,
                0.65F);
        const float backward =
            HenyeyGreensteinPhase(
                -1.0F,
                0.65F);

        Check(
            forward > isotropic);
        Check(
            forward > backward);
    }

    return 0;
}

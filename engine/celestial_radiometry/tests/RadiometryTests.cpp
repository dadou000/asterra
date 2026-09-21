#include <orbit/celestial_radiometry/Radiometry.hpp>

#include <cmath>

int main()
{
    using namespace orbit::celestial_radiometry;

    const auto sun =
        Resolve({
            .radiusMeters = 6.957e8,
            .effectiveTemperatureKelvin = 5772.0,
            .emissivity = 1.0,
            .explicitLuminosityWatts = 0.0,
            .deriveLuminosity = true
        });

    if (std::abs(
            sun.luminosityWatts -
            3.828e26) /
            3.828e26 >
        0.01)
    {
        return 1;
    }

    constexpr double au =
        149'597'870'700.0;

    const double solarIrradiance =
        IrradianceWattsPerSquareMeter(
            3.828e26,
            au);

    if (std::abs(
            solarIrradiance -
            1361.0) >
        2.0)
    {
        return 2;
    }

    const double pixelOmega =
        CentralPixelSolidAngleSteradians(
            1.0,
            1000U);

    if (!(pixelOmega > 0.0) ||
        std::abs(
            ResolvedPixelIrradianceWattsPerSquareMeter(
                2.0,
                1.0,
                1000U) -
            2.0 * pixelOmega) >
            1.0e-15)
    {
        return 6;
    }

    if (std::abs(
            EncodeIrradianceSceneLinear(1361.0) -
            0.18) >
        1.0e-12)
    {
        return 7;
    }

    const auto exposure =
        ResolveExposure({});

    if (std::abs(
            ExposeIrradiance(
                exposure.referenceIrradianceWattsPerSquareMeter,
                exposure) -
            exposure.middleGray) >
        1.0e-12)
    {
        return 3;
    }

    const auto plusOne =
        ResolveExposure({
            .referenceIrradianceWattsPerSquareMeter = 1361.0,
            .middleGray = 0.18,
            .compensationStops = 1.0
        });

    if (std::abs(
            plusOne.scalePerWattPerSquareMeter /
            exposure.scalePerWattPerSquareMeter -
            2.0) >
        1.0e-12)
    {
        return 4;
    }

    if (ToneMapReinhard(0.0) != 0.0 ||
        ToneMapReinhard(1.0) != 0.5 ||
        ToneMapReinhard(1000.0) <= 0.99)
    {
        return 5;
    }

    return 0;
}

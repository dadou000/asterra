#include <orbit/volume_solver/SurfaceVolumeSolver.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <vector>

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
            << "Surface-volume solver test failed at "
            << location.file_name()
            << ':'
            << location.line()
            << '\n';
        std::exit(1);
    }
}
} // namespace

int main()
{
    using namespace orbit;
    using namespace orbit::volume_solver;

    SurfaceVolumeReferenceConfig config{
        .width = 5U,
        .height = 1U,
        .layers = 1U,
        .cellSizeX = 1.0F,
        .cellSizeY = 1.0F,
        .cellSizeZ = 1.0F,
        .deltaSeconds = 0.25F,
        .scalarDissipationPerSecond = 0.0F,
        .velocityDissipationPerSecond = 0.0F
    };

    std::vector<SurfaceVolumeCell>
        input(5U);
    std::vector<SurfaceVolumeCell>
        output(5U);

    input[1].scalar = 1.0F;

    for (auto& cell : input)
    {
        cell.velocity = {
            1.0F, 0.0F, 0.0F};
    }

    StepSurfaceVolumeReference(
        input,
        output,
        config);

    // Positive X flow advects density from cell 1 toward cell 2.
    Check(output[1].scalar < input[1].scalar);
    Check(output[2].scalar > input[2].scalar);
    Check(output[0].scalar >= 0.0F);
    Check(output[4].scalar >= 0.0F);

    config.scalarDissipationPerSecond =
        2.0F;
    config.velocityDissipationPerSecond =
        1.0F;

    std::vector<SurfaceVolumeCell>
        dissipated(5U);

    StepSurfaceVolumeReference(
        input,
        dissipated,
        config);

    Check(dissipated[1].scalar <
        output[1].scalar);
    Check(std::abs(
              dissipated[1].velocity.x) <
          std::abs(
              output[1].velocity.x));

    std::vector<SurfaceVolumeCell>
        windDriven(5U);

    StepSurfaceVolumeReference(
        input,
        windDriven,
        SurfaceVolumeReferenceConfig{
            .width = 5U,
            .height = 1U,
            .layers = 1U,
            .cellSizeX = 1.0F,
            .cellSizeY = 1.0F,
            .cellSizeZ = 1.0F,
            .deltaSeconds = 0.25F
        },
        {2.0F, 0.0F, 0.0F});

    Check(windDriven[3].velocity.x >
        output[3].velocity.x);

    // A compact 2.5D stack remains independent from screen dimensions and
    // supports more than one surface-normal layer.
    SurfaceVolumeReferenceConfig layered{
        .width = 2U,
        .height = 2U,
        .layers = 3U,
        .cellSizeX = 2.0F,
        .cellSizeY = 0.5F,
        .cellSizeZ = 2.0F,
        .deltaSeconds = 0.1F
    };

    std::vector<SurfaceVolumeCell>
        layeredInput(12U);
    std::vector<SurfaceVolumeCell>
        layeredOutput(12U);

    layeredInput[0].scalar = 1.0F;
    layeredInput[0].velocity = {
        0.0F, 1.0F, 0.0F};

    StepSurfaceVolumeReference(
        layeredInput,
        layeredOutput,
        layered);

    for (const auto& cell :
         layeredOutput)
    {
        Check(std::isfinite(cell.scalar));
        Check(std::isfinite(cell.velocity.x));
        Check(std::isfinite(cell.velocity.y));
        Check(std::isfinite(cell.velocity.z));
    }

    return 0;
}

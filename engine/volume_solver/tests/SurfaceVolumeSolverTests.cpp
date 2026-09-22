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

    SurfaceVolumeReferenceConfig local3DConfig{
        .width = 3U,
        .height = 3U,
        .layers = 3U,
        .cellSizeX = 1.0F,
        .cellSizeY = 1.0F,
        .cellSizeZ = 1.0F,
        .deltaSeconds = 0.2F
    };

    std::vector<LocalVolumeReferenceCell>
        localInput(27U);
    std::vector<LocalVolumeReferenceCell>
        localOutput(27U);

    const auto localIndex =
        [&](const u32 x,
            const u32 y,
            const u32 z)
        {
            return
                (static_cast<std::size_t>(z) *
                     local3DConfig.layers +
                 y) *
                    local3DConfig.width +
                x;
        };

    const auto center =
        localIndex(1U,1U,1U);
    const auto above =
        localIndex(1U,2U,1U);

    localInput[center].density = 1.0F;
    localInput[center].temperature = 10.0F;

    for (auto& cell :
         localInput)
    {
        cell.velocity = {
            0.0F, 1.0F, 0.0F};
    }

    StepLocalVolumeReference(
        localInput,
        localOutput,
        local3DConfig);

    // True vertical transport: both independent scalar channels move along Y.
    Check(localOutput[center].density <
        localInput[center].density);
    Check(localOutput[above].density > 0.0F);
    Check(localOutput[center].temperature <
        localInput[center].temperature);
    Check(localOutput[above].temperature > 0.0F);

    // Velocity remains a full 3-component transported field.
    Check(std::isfinite(
        localOutput[center].velocity.x));
    Check(std::isfinite(
        localOutput[center].velocity.y));
    Check(std::isfinite(
        localOutput[center].velocity.z));
    Check(localOutput[center].velocity.y > 0.0F);

    return 0;
}

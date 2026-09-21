#include <orbit/lighting/RadianceClipmap.hpp>

#include <cmath>

int main()
{
    using namespace orbit;
    using namespace orbit::lighting;

    const RadianceClipmapConfig config{
        .baseCellSizeMeters = 2.0,
        .levelScale = 4.0,
        .levelCount = 4U,
        .cellsPerAxis = 8U
    };

    const auto memory =
        BuildRadianceClipmapMemoryLayout(
            config);

    if (memory.bytesPerCell != 64U ||
        memory.levels.size() != 4U ||
        memory.levels[0].cellSizeMeters != 2.0 ||
        memory.levels[1].cellSizeMeters != 8.0 ||
        memory.levels[2].cellSizeMeters != 32.0 ||
        memory.levels[3].cellSizeMeters != 128.0)
    {
        return 1;
    }

    // 8^3 cells * 64 bytes = 32768 bytes/level.
    if (memory.levels[0].cellCount != 512U ||
        memory.levels[0].byteSize != 32'768U ||
        memory.totalBytes != 131'072U)
    {
        return 2;
    }

    LightingView view;
    view.frame = frames::FrameId{
        .high = 1U,
        .low = 2U};
    view.body = universe::BodyId{
        .high = 3U,
        .low = 4U};
    view.gpuOriginInFrameMeters = {
        1000.0,
        -2000.0,
        3000.0
    };

    const math::Double3 point{
        17.5,
        -1.0,
        64.1
    };

    const auto keyBefore =
        RadianceCellForPoint(
            point,
            config,
            1U,
            view);

    view.gpuOriginInFrameMeters = {
        -50'000.0,
        80'000.0,
        10.0
    };
    ++view.gpuOriginRevision;

    const auto keyAfter =
        RadianceCellForPoint(
            point,
            config,
            1U,
            view);

    if (keyBefore != keyAfter)
    {
        return 3;
    }

    const auto center =
        RadianceCellCenterInFrame(
            keyBefore,
            config);

    if (std::abs(center.x - 20.0) > 1.0e-9 ||
        std::abs(center.y - (-4.0)) > 1.0e-9 ||
        std::abs(center.z - 68.0) > 1.0e-9)
    {
        return 4;
    }

    const auto gpuCenter =
        RadianceCellGpuCenter(
            keyBefore,
            config,
            view);

    if (std::abs(
            gpuCenter.x -
            static_cast<f32>(
                center.x -
                view.gpuOriginInFrameMeters.x)) >
            1.0e-3F)
    {
        return 5;
    }

    const DirectionalIrradianceL1 irradiance{
        .l0 = {1.0F, 1.0F, 1.0F},
        .l1x = {0.5F, 0.0F, 0.0F},
        .l1y = {0.0F, 0.25F, 0.0F},
        .l1z = {0.0F, 0.0F, 0.75F}
    };

    const auto positiveX =
        EvaluateIrradiance(
            irradiance,
            {1.0F, 0.0F, 0.0F});

    const auto negativeX =
        EvaluateIrradiance(
            irradiance,
            {-1.0F, 0.0F, 0.0F});

    if (positiveX.x <= negativeX.x ||
        std::abs(positiveX.y - 1.0F) >
            1.0e-6F)
    {
        return 6;
    }

    const RadianceCell cell{
        .irradiance = irradiance,
        .revision = 0x123456U,
        .updateAgeSeconds = 2.5F,
        .sampleCount = 32U,
        .valid = true
    };

    const auto gpu =
        EncodeGpuRadianceCell(cell);

    if (sizeof(gpu) != 64U ||
        gpu.irradiance0.w != 1.0F ||
        gpu.irradianceX.w != 2.5F ||
        gpu.irradianceY.w != 32.0F ||
        gpu.irradianceZ.w !=
            static_cast<f32>(0x123456U))
    {
        return 7;
    }

    return 0;
}

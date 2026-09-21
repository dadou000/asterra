#include <orbit/lighting/PlanetaryEmissionField.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace orbit::lighting
{
namespace
{
[[nodiscard]] u32 WrapLongitude(
    const f64 longitude,
    const u32 width) noexcept
{
    constexpr f64 kTwoPi =
        6.28318530717958647692;

    f64 normalized =
        longitude / kTwoPi +
        0.5;

    normalized -=
        std::floor(normalized);

    return
        std::min(
            static_cast<u32>(
                normalized *
                static_cast<f64>(width)),
            width - 1U);
}

[[nodiscard]] u32 LatitudeIndex(
    const f64 latitude,
    const u32 height) noexcept
{
    constexpr f64 kPi =
        3.14159265358979323846;

    const f64 normalized =
        std::clamp(
            latitude / kPi +
            0.5,
            0.0,
            1.0);

    return
        std::min(
            static_cast<u32>(
                normalized *
                static_cast<f64>(height)),
            height - 1U);
}

[[nodiscard]] std::size_t CellIndex(
    const u32 x,
    const u32 y,
    const u32 width) noexcept
{
    return
        static_cast<std::size_t>(y) *
            width +
        x;
}

void Accumulate(
    PlanetaryEmissionCell& destination,
    const PlanetaryEmissionCell& source) noexcept
{
    destination.integratedRadianceArea.x +=
        source.integratedRadianceArea.x;
    destination.integratedRadianceArea.y +=
        source.integratedRadianceArea.y;
    destination.integratedRadianceArea.z +=
        source.integratedRadianceArea.z;
    destination.radiantImportance +=
        source.radiantImportance;
    destination.contributingEmitters +=
        source.contributingEmitters;
}
} // namespace

PlanetaryEmissionField
BuildPlanetaryEmissionField(
    const frames::FrameId frame,
    const universe::BodyId body,
    const math::Double3& bodyCenterInFrameMeters,
    const u64 sourceRevision,
    const std::span<
        const EmissiveSampledEmitter> emitters,
    const PlanetaryEmissionFieldConfig& config)
{
    if (!frame ||
        !body ||
        config.baseWidth == 0U ||
        config.baseHeight == 0U ||
        config.maximumLevels == 0U)
    {
        throw std::invalid_argument(
            "Planetary emission field requires valid authority and resolution.");
    }

    PlanetaryEmissionField result{
        .frame = frame,
        .body = body,
        .bodyCenterInFrameMeters =
            bodyCenterInFrameMeters,
        .sourceRevision = sourceRevision
    };

    PlanetaryEmissionLevel base{
        .width = config.baseWidth,
        .height = config.baseHeight,
        .cells =
            std::vector<PlanetaryEmissionCell>(
                static_cast<std::size_t>(
                    config.baseWidth) *
                config.baseHeight)
    };

    for (const auto& emitter : emitters)
    {
        const auto delta =
            emitter.positionInFrameMeters -
            bodyCenterInFrameMeters;

        const f64 length =
            math::Length(delta);

        if (!std::isfinite(length) ||
            length <= 1.0e-9)
        {
            continue;
        }

        const auto direction =
            delta / length;

        const f64 longitude =
            std::atan2(
                direction.z,
                direction.x);

        const f64 latitude =
            std::asin(
                std::clamp(
                    direction.y,
                    -1.0,
                    1.0));

        const u32 x =
            WrapLongitude(
                longitude,
                base.width);
        const u32 y =
            LatitudeIndex(
                latitude,
                base.height);

        auto& cell =
            base.cells[
                CellIndex(
                    x,
                    y,
                    base.width)];

        cell.integratedRadianceArea.x +=
            std::max(
                emitter.
                    integratedRadianceArea.x,
                0.0F) *
            std::max(
                emitter.estimatorWeight,
                0.0F);
        cell.integratedRadianceArea.y +=
            std::max(
                emitter.
                    integratedRadianceArea.y,
                0.0F) *
            std::max(
                emitter.estimatorWeight,
                0.0F);
        cell.integratedRadianceArea.z +=
            std::max(
                emitter.
                    integratedRadianceArea.z,
                0.0F) *
            std::max(
                emitter.estimatorWeight,
                0.0F);

        cell.radiantImportance +=
            std::max(
                emitter.radiantImportance,
                0.0) *
            static_cast<f64>(
                std::max(
                    emitter.estimatorWeight,
                    0.0F));

        ++cell.contributingEmitters;
    }

    result.levels.push_back(
        std::move(base));

    while (result.levels.size() <
               config.maximumLevels)
    {
        const auto& previous =
            result.levels.back();

        if (previous.width == 1U &&
            previous.height == 1U)
        {
            break;
        }

        PlanetaryEmissionLevel next{
            .width =
                std::max(
                    previous.width / 2U,
                    1U),
            .height =
                std::max(
                    previous.height / 2U,
                    1U)
        };

        next.cells.resize(
            static_cast<std::size_t>(
                next.width) *
            next.height);

        for (u32 y = 0U;
             y < previous.height;
             ++y)
        {
            for (u32 x = 0U;
                 x < previous.width;
                 ++x)
            {
                const u32 parentX =
                    std::min(
                        x / 2U,
                        next.width - 1U);
                const u32 parentY =
                    std::min(
                        y / 2U,
                        next.height - 1U);

                Accumulate(
                    next.cells[
                        CellIndex(
                            parentX,
                            parentY,
                            next.width)],
                    previous.cells[
                        CellIndex(
                            x,
                            y,
                            previous.width)]);
            }
        }

        result.levels.push_back(
            std::move(next));
    }

    return result;
}

const PlanetaryEmissionCell*
SamplePlanetaryEmission(
    const PlanetaryEmissionField& field,
    const u32 level,
    const math::Double3& directionFromBodyCenter) noexcept
{
    if (level >= field.levels.size())
    {
        return nullptr;
    }

    const f64 length =
        math::Length(
            directionFromBodyCenter);

    if (!std::isfinite(length) ||
        length <= 1.0e-12)
    {
        return nullptr;
    }

    const auto direction =
        directionFromBodyCenter /
        length;

    const auto& map =
        field.levels[level];

    const f64 longitude =
        std::atan2(
            direction.z,
            direction.x);

    const f64 latitude =
        std::asin(
            std::clamp(
                direction.y,
                -1.0,
                1.0));

    const u32 x =
        WrapLongitude(
            longitude,
            map.width);
    const u32 y =
        LatitudeIndex(
            latitude,
            map.height);

    return
        &map.cells[
            CellIndex(
                x,
                y,
                map.width)];
}

math::Float3
TotalPlanetaryIntegratedRadianceArea(
    const PlanetaryEmissionLevel& level) noexcept
{
    math::Float3 result{};

    for (const auto& cell : level.cells)
    {
        result.x +=
            cell.integratedRadianceArea.x;
        result.y +=
            cell.integratedRadianceArea.y;
        result.z +=
            cell.integratedRadianceArea.z;
    }

    return result;
}
} // namespace orbit::lighting

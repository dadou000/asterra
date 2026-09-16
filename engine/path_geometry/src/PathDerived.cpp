#include <orbit/path_geometry/PathDerived.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace orbit::path_geometry
{
namespace
{
constexpr f64 kEpsilon = 1.0e-9;

[[nodiscard]] bool Finite(
    const math::Double3& value) noexcept
{
    return
        std::isfinite(value.x) &&
        std::isfinite(value.y) &&
        std::isfinite(value.z);
}

[[nodiscard]] u64 Mix(
    u64 hash,
    const u64 value) noexcept
{
    hash ^=
        value +
        0x9e3779b97f4a7c15ULL +
        (hash << 6U) +
        (hash >> 2U);
    return hash;
}

[[nodiscard]] u64 HashDouble(
    const f64 value) noexcept
{
    return std::bit_cast<u64>(value);
}

[[nodiscard]] u64 HashPoint(
    u64 hash,
    const math::Double3& value) noexcept
{
    hash = Mix(hash, HashDouble(value.x));
    hash = Mix(hash, HashDouble(value.y));
    hash = Mix(hash, HashDouble(value.z));
    return hash;
}

[[nodiscard]] math::Double3 SafeUp(
    math::Double3 up,
    const math::Double3& tangent) noexcept
{
    up = math::Normalize(up);

    if (math::LengthSquared(up) <= kEpsilon)
    {
        up = {0.0, 1.0, 0.0};
    }

    if (std::abs(
            math::Dot(up, tangent)) > 0.98)
    {
        up =
            std::abs(tangent.y) < 0.98
                ? math::Double3{0.0, 1.0, 0.0}
                : math::Double3{1.0, 0.0, 0.0};
    }

    return math::Normalize(up);
}

[[nodiscard]] math::Double3 SafeLateral(
    const math::Double3& tangent,
    const math::Double3& requestedUp) noexcept
{
    const math::Double3 up =
        SafeUp(requestedUp, tangent);

    math::Double3 lateral =
        math::Normalize(
            math::Cross(up, tangent));

    if (math::LengthSquared(lateral) <=
        kEpsilon)
    {
        const math::Double3 fallback =
            std::abs(tangent.z) < 0.98
                ? math::Double3{0.0, 0.0, 1.0}
                : math::Double3{1.0, 0.0, 0.0};

        lateral =
            math::Normalize(
                math::Cross(
                    fallback,
                    tangent));
    }

    return lateral;
}

struct SourceSpan
{
    PathCenterlineSample a;
    PathCenterlineSample b;
    f64 startMeters{0.0};
    f64 endMeters{0.0};
};

[[nodiscard]] std::vector<PathStation>
Resample(
    const PathCenterline& centerline,
    const f64 spacing)
{
    std::vector<SourceSpan> spans;
    spans.reserve(
        centerline.samples.size() - 1U);

    f64 total = 0.0;

    for (std::size_t index = 1;
         index < centerline.samples.size();
         ++index)
    {
        const auto& a =
            centerline.samples[index - 1U];
        const auto& b =
            centerline.samples[index];

        if (!Finite(a.position) ||
            !Finite(b.position) ||
            !Finite(a.up) ||
            !Finite(b.up))
        {
            throw std::invalid_argument(
                "Path centerline contains non-finite values.");
        }

        const f64 length =
            math::Length(
                b.position - a.position);

        if (length <= kEpsilon)
        {
            continue;
        }

        spans.push_back({
            .a = a,
            .b = b,
            .startMeters = total,
            .endMeters = total + length
        });

        total += length;
    }

    if (spans.empty() ||
        total <= kEpsilon)
    {
        throw std::invalid_argument(
            "Path centerline requires at least two distinct samples.");
    }

    std::vector<f64> requestedStations;

    for (f64 station = 0.0;
         station < total;
         station += spacing)
    {
        requestedStations.push_back(
            station);
    }

    if (requestedStations.empty() ||
        std::abs(
            requestedStations.back() -
            total) > kEpsilon)
    {
        requestedStations.push_back(
            total);
    }

    std::vector<PathStation> result;
    result.reserve(
        requestedStations.size());

    std::size_t spanIndex = 0;

    for (const f64 station :
         requestedStations)
    {
        while (spanIndex + 1U <
                   spans.size() &&
               station >
                   spans[spanIndex].
                       endMeters)
        {
            ++spanIndex;
        }

        const SourceSpan& span =
            spans[spanIndex];

        const f64 length =
            span.endMeters -
            span.startMeters;

        const f64 t =
            length <= kEpsilon
                ? 0.0
                : std::clamp(
                      (station -
                       span.startMeters) /
                          length,
                      0.0,
                      1.0);

        const math::Double3 position =
            span.a.position *
                (1.0 - t) +
            span.b.position * t;

        const math::Double3 up =
            math::Normalize(
                span.a.up *
                    (1.0 - t) +
                span.b.up * t);

        result.push_back({
            .stationMeters = station,
            .position = position,
            .up =
                math::LengthSquared(up) >
                        kEpsilon
                    ? up
                    : math::Double3{
                          0.0,
                          1.0,
                          0.0}
        });
    }

    for (std::size_t index = 0;
         index < result.size();
         ++index)
    {
        math::Double3 tangent;

        if (index == 0)
        {
            tangent =
                result[1].position -
                result[0].position;
        }
        else if (index + 1U ==
                 result.size())
        {
            tangent =
                result[index].position -
                result[index - 1U].
                    position;
        }
        else
        {
            tangent =
                result[index + 1U].
                    position -
                result[index - 1U].
                    position;
        }

        tangent =
            math::Normalize(tangent);

        if (math::LengthSquared(tangent) <=
            kEpsilon)
        {
            throw std::runtime_error(
                "Path resampling produced a degenerate tangent.");
        }

        const math::Double3 lateral =
            SafeLateral(
                tangent,
                result[index].up);

        math::Double3 up =
            math::Normalize(
                math::Cross(
                    tangent,
                    lateral));

        if (math::Dot(
                up,
                result[index].up) < 0.0)
        {
            up = up * -1.0;
        }

        result[index].tangent = tangent;
        result[index].lateral = lateral;
        result[index].up = up;
    }

    return result;
}

[[nodiscard]] u64 BuildSignature(
    const PathCenterline& centerline,
    const paths::PathProfile& profile,
    const PathBuildOptions& options)
    noexcept
{
    u64 hash =
        0x243f6a8885a308d3ULL;

    hash = Mix(hash, centerline.edge.high);
    hash = Mix(hash, centerline.edge.low);
    hash = Mix(hash, centerline.frame.high);
    hash = Mix(hash, centerline.frame.low);
    hash = Mix(
        hash,
        centerline.sourceRevision);
    hash = Mix(
        hash,
        HashDouble(
            profile.widthMeters));
    hash = Mix(
        hash,
        static_cast<u64>(
            profile.lanes));
    hash = Mix(
        hash,
        HashDouble(
            options.sampleSpacingMeters));

    for (const auto& sample :
         centerline.samples)
    {
        hash =
            HashPoint(
                hash,
                sample.position);
        hash =
            HashPoint(
                hash,
                sample.up);
    }

    return hash;
}
} // namespace

PathDerivedProduct BuildPathDerived(
    const PathCenterline& centerline,
    const paths::PathProfile& profile,
    const PathBuildOptions options)
{
    if (!centerline.edge)
    {
        throw std::invalid_argument(
            "Derived path requires a valid semantic edge ID.");
    }

    if (!centerline.frame)
    {
        throw std::invalid_argument(
            "Derived path requires an explicit frame.");
    }

    if (centerline.samples.size() < 2U)
    {
        throw std::invalid_argument(
            "Derived path requires at least two centerline samples.");
    }

    if (!std::isfinite(
            profile.widthMeters) ||
        profile.widthMeters <= 0.0)
    {
        throw std::invalid_argument(
            "Path profile width must be positive and finite.");
    }

    if (!std::isfinite(
            options.sampleSpacingMeters) ||
        options.sampleSpacingMeters <= 0.0)
    {
        throw std::invalid_argument(
            "Derived path sample spacing must be positive and finite.");
    }

    PathDerivedProduct product{
        .edge = centerline.edge,
        .frame = centerline.frame,
        .sourceRevision =
            centerline.sourceRevision,
        .buildSignature =
            BuildSignature(
                centerline,
                profile,
                options),
        .widthMeters =
            profile.widthMeters,
        .laneCount =
            profile.lanes,
        .stations =
            Resample(
                centerline,
                options.
                    sampleSpacingMeters)
    };

    const f64 halfWidth =
        profile.widthMeters * 0.5;

    product.visualMesh.vertices.reserve(
        product.stations.size() * 2U);

    for (const PathStation& station :
         product.stations)
    {
        product.visualMesh.vertices.push_back({
            .position =
                station.position -
                station.lateral *
                    halfWidth,
            .normal = station.up,
            .uv = {
                0.0,
                station.stationMeters
            }
        });

        product.visualMesh.vertices.push_back({
            .position =
                station.position +
                station.lateral *
                    halfWidth,
            .normal = station.up,
            .uv = {
                1.0,
                station.stationMeters
            }
        });

        product.navigation.push_back({
            .stationMeters =
                station.stationMeters,
            .position =
                station.position,
            .tangent =
                station.tangent,
            .halfWidthMeters =
                halfWidth,
            .lanes =
                profile.lanes
        });
    }

    if (product.stations.size() >= 2U)
    {
        const std::size_t segmentCount =
            product.stations.size() - 1U;

        product.visualMesh.indices.reserve(
            segmentCount * 6U);
        product.collision.reserve(
            segmentCount * 2U);
        product.debugLines.reserve(
            segmentCount *
            (3U +
             static_cast<std::size_t>(
                 profile.lanes)));

        for (std::size_t index = 0;
             index < segmentCount;
             ++index)
        {
            const u32 l0 =
                static_cast<u32>(
                    index * 2U);
            const u32 r0 = l0 + 1U;
            const u32 l1 = l0 + 2U;
            const u32 r1 = l0 + 3U;

            const u32 triangleIndices[6]{
                l0, r0, l1,
                r0, r1, l1
            };

            for (const u32 value :
                 triangleIndices)
            {
                product.visualMesh.indices.
                    push_back(value);
            }

            const auto vertexPosition =
                [&product](const u32 vertex)
                {
                    return product.
                        visualMesh.
                        vertices[vertex].
                        position;
                };

            product.collision.push_back({
                .a = vertexPosition(l0),
                .b = vertexPosition(r0),
                .c = vertexPosition(l1)
            });

            product.collision.push_back({
                .a = vertexPosition(r0),
                .b = vertexPosition(r1),
                .c = vertexPosition(l1)
            });

            product.debugLines.push_back({
                .start =
                    vertexPosition(l0),
                .end =
                    vertexPosition(l1),
                .kind =
                    DebugLineKind::Boundary
            });

            product.debugLines.push_back({
                .start =
                    vertexPosition(r0),
                .end =
                    vertexPosition(r1),
                .kind =
                    DebugLineKind::Boundary
            });

            product.debugLines.push_back({
                .start =
                    product.stations[index].
                        position,
                .end =
                    product.stations[index + 1U].
                        position,
                .kind =
                    DebugLineKind::Centerline
            });
        }
    }

    if (profile.lanes > 0U)
    {
        const f64 laneWidth =
            profile.widthMeters /
            static_cast<f64>(
                profile.lanes);

        product.lanes.reserve(
            profile.lanes);

        for (u32 lane = 0;
             lane < profile.lanes;
             ++lane)
        {
            const f64 offset =
                -halfWidth +
                laneWidth *
                    (static_cast<f64>(
                         lane) +
                     0.5);

            LaneReference reference{
                .laneIndex = lane,
                .lateralOffsetMeters =
                    offset
            };

            reference.points.reserve(
                product.stations.size());

            const u32 nodeBase =
                static_cast<u32>(
                    product.referenceGraph.
                        nodes.size());

            for (std::size_t index = 0;
                 index <
                     product.stations.size();
                 ++index)
            {
                const PathStation& station =
                    product.stations[index];

                const math::Double3 position =
                    station.position +
                    station.lateral *
                        offset;

                reference.points.push_back({
                    .stationMeters =
                        station.stationMeters,
                    .position = position,
                    .tangent =
                        station.tangent
                });

                product.referenceGraph.
                    nodes.push_back({
                        .index =
                            static_cast<u32>(
                                product.
                                    referenceGraph.
                                    nodes.size()),
                        .laneIndex = lane,
                        .stationMeters =
                            station.
                                stationMeters,
                        .position =
                            position,
                        .tangent =
                            station.tangent
                    });

                if (index > 0U)
                {
                    const u32 from =
                        nodeBase +
                        static_cast<u32>(
                            index - 1U);
                    const u32 to =
                        nodeBase +
                        static_cast<u32>(
                            index);

                    product.referenceGraph.
                        edges.push_back({
                            .from = from,
                            .to = to,
                            .lengthMeters =
                                math::Length(
                                    product.
                                        referenceGraph.
                                        nodes[to].
                                        position -
                                    product.
                                        referenceGraph.
                                        nodes[from].
                                        position),
                            .bidirectional =
                                true
                        });
                }
            }

            for (std::size_t index = 1;
                 index <
                     reference.points.size();
                 ++index)
            {
                product.debugLines.push_back({
                    .start =
                        reference.
                            points[index - 1U].
                            position,
                    .end =
                        reference.
                            points[index].
                            position,
                    .kind =
                        DebugLineKind::Lane
                });
            }

            product.lanes.push_back(
                std::move(reference));
        }
    }

    return product;
}
} // namespace orbit::path_geometry

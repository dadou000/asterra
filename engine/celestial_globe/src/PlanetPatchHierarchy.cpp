#include <orbit/celestial_globe/PlanetPatchHierarchy.hpp>

#include <orbit/terrain/TerrainContracts.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <execution>
#include <limits>
#include <map>
#include <numbers>
#include <numeric>
#include <queue>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace orbit::celestial_globe
{
namespace
{
[[nodiscard]] f64 ReferenceRadius(
    const universe::BodyShape& shape)
{
    return std::visit(
        [](const auto& value) -> f64
        {
            using Shape = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Shape, universe::SphereShape>)
            {
                return value.radiusMeters;
            }
            else
            {
                return std::max({
                    value.radiiMeters.x,
                    value.radiiMeters.y,
                    value.radiiMeters.z
                });
            }
        },
        shape);
}

[[nodiscard]] f64 RadiusAlong(
    const universe::BodyShape& shape,
    const math::Double3 direction)
{
    return std::visit(
        [&direction](const auto& value) -> f64
        {
            using Shape = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Shape, universe::SphereShape>)
            {
                return value.radiusMeters;
            }
            else
            {
                const f64 x = direction.x / value.radiiMeters.x;
                const f64 y = direction.y / value.radiiMeters.y;
                const f64 z = direction.z / value.radiiMeters.z;
                return 1.0 / std::sqrt(x*x + y*y + z*z);
            }
        },
        shape);
}

[[nodiscard]] math::Double3 FaceDirection(
    const u32 face,
    const f64 u,
    const f64 v)
{
    math::Double3 p{};
    switch (face)
    {
    case 0U: p = { 1.0, v, -u}; break;
    case 1U: p = {-1.0, v,  u}; break;
    case 2U: p = { u, 1.0, -v}; break;
    case 3U: p = { u,-1.0,  v}; break;
    case 4U: p = { u, v, 1.0}; break;
    default: p = {-u, v,-1.0}; break;
    }
    return math::Normalize(p);
}

struct PatchUvBounds
{
    f64 u0{-1.0};
    f64 v0{-1.0};
    f64 u1{1.0};
    f64 v1{1.0};
};

[[nodiscard]] PatchUvBounds UvBounds(
    const PlanetPatchId id)
{
    const u64 subdivisions = 1ULL << id.level;
    const f64 span = 2.0 / static_cast<f64>(subdivisions);
    return {
        .u0 = -1.0 + static_cast<f64>(id.x) * span,
        .v0 = -1.0 + static_cast<f64>(id.y) * span,
        .u1 = -1.0 + static_cast<f64>(id.x + 1U) * span,
        .v1 = -1.0 + static_cast<f64>(id.y + 1U) * span
    };
}

[[nodiscard]] f64 ClampUnit(const f64 value) noexcept
{
    return std::clamp(value, -1.0, 1.0);
}

[[nodiscard]] bool ValidConfig(
    const PlanetPatchSelectorConfig& config) noexcept
{
    return
        config.patchResolution >= 3U &&
        config.maximumLevel <= 24U &&
        std::isfinite(config.targetCellPixels) &&
        config.targetCellPixels > 0.0 &&
        std::isfinite(config.hysteresisFraction) &&
        config.hysteresisFraction >= 0.0 &&
        config.hysteresisFraction < 0.95 &&
        config.maximumSelectedPatches >= 6U;
}

struct EvaluatedPatch
{
    PlanetPatchId id{};
    f64 cellPixels{0.0};
    bool visible{true};
    bool horizonCulled{false};
    bool frustumCulled{false};
};

[[nodiscard]] EvaluatedPatch EvaluatePatch(
    const PlanetPatchId id,
    const f64 referenceRadius,
    const PlanetPatchView& view,
    const PlanetPatchSelectorConfig& config)
{
    EvaluatedPatch result{.id = id};
    const auto uv = UvBounds(id);
    const f64 uc = (uv.u0 + uv.u1) * 0.5;
    const f64 vc = (uv.v0 + uv.v1) * 0.5;
    const auto centerDirection = FaceDirection(id.face, uc, vc);

    const std::array<math::Double3, 4> corners{
        FaceDirection(id.face, uv.u0, uv.v0),
        FaceDirection(id.face, uv.u1, uv.v0),
        FaceDirection(id.face, uv.u0, uv.v1),
        FaceDirection(id.face, uv.u1, uv.v1)
    };

    f64 angularRadius = 0.0;
    for (const auto corner : corners)
    {
        angularRadius = std::max(
            angularRadius,
            std::acos(ClampUnit(math::Dot(centerDirection, corner))));
    }

    const f64 displacement = std::max(view.maximumDisplacementMeters, 0.0);
    const f64 outerRadius = referenceRadius + displacement;
    const auto centerMeters = centerDirection * referenceRadius;
    const f64 boundRadius =
        outerRadius * std::sin(std::min(angularRadius, std::numbers::pi_v<f64> * 0.5)) +
        displacement;

    const auto cameraToCenter = centerMeters - view.cameraPositionMeters;
    const f64 distanceToCenter = math::Length(cameraToCenter);

    if (config.horizonCulling)
    {
        const f64 cameraRadius = math::Length(view.cameraPositionMeters);
        if (cameraRadius > outerRadius + 1.0)
        {
            const auto cameraDirection = math::Normalize(view.cameraPositionMeters);
            const f64 centerAngle = std::acos(
                ClampUnit(math::Dot(cameraDirection, centerDirection)));

            // Use the inner reference radius for a conservative horizon. The
            // node cone and displacement margin expand it further so a tall
            // feature cannot be incorrectly discarded near the limb.
            const f64 horizonAngle = std::acos(
                ClampUnit(referenceRadius / cameraRadius));
            const f64 displacementAngle =
                std::asin(std::clamp(
                    displacement / std::max(cameraRadius, 1.0),
                    0.0,
                    1.0));

            if (centerAngle > horizonAngle + angularRadius + displacementAngle)
            {
                result.visible = false;
                result.horizonCulled = true;
                return result;
            }
        }
    }

    if (config.frustumCulling && distanceToCenter > 1.0e-6)
    {
        const auto toCenter = cameraToCenter / distanceToCenter;
        const math::Double3 forward{
            static_cast<f64>(view.cameraForward.x),
            static_cast<f64>(view.cameraForward.y),
            static_cast<f64>(view.cameraForward.z)
        };
        const auto forwardUnit =
            math::LengthSquared(forward) > 1.0e-20
                ? math::Normalize(forward)
                : math::Double3{0.0, 0.0, 1.0};

        const f64 aspect =
            static_cast<f64>(std::max(view.viewportWidthPixels, 1U)) /
            static_cast<f64>(std::max(view.viewportHeightPixels, 1U));
        const f64 halfVertical =
            std::clamp(view.verticalFovRadians * 0.5, 0.01, 1.55);
        const f64 tanVertical = std::tan(halfVertical);
        const f64 halfHorizontal = std::atan(tanVertical * aspect);
        const f64 diagonalHalf = std::atan(
            std::sqrt(
                tanVertical * tanVertical +
                std::tan(halfHorizontal) * std::tan(halfHorizontal)));
        const f64 patchAngularRadius = std::asin(
            std::clamp(
                boundRadius / distanceToCenter,
                0.0,
                1.0));
        const f64 viewAngle = std::acos(
            ClampUnit(math::Dot(forwardUnit, toCenter)));

        if (viewAngle > diagonalHalf + patchAngularRadius)
        {
            result.visible = false;
            result.frustumCulled = true;
            return result;
        }
    }

    const f64 focalPixels =
        static_cast<f64>(std::max(view.viewportHeightPixels, 1U)) /
        (2.0 * std::tan(
            std::clamp(view.verticalFovRadians * 0.5, 0.01, 1.55)));

    // The bounding diameter deliberately overestimates a cube-sphere node's
    // projected grid-cell spacing. Over-refinement is preferable to visible
    // orbital faceting, and the hard patch budget prevents runaway work.
    const f64 cellWorldMeters =
        (2.0 * boundRadius) /
        static_cast<f64>(config.patchResolution - 1U);
    result.cellPixels =
        focalPixels * cellWorldMeters /
        std::max(distanceToCenter - boundRadius, referenceRadius * 1.0e-6);

    return result;
}

[[nodiscard]] PlanetPatchId ParentOf(const PlanetPatchId id) noexcept
{
    return {
        .face = id.face,
        .level = static_cast<u8>(id.level - 1U),
        .x = id.x / 2U,
        .y = id.y / 2U
    };
}

[[nodiscard]] std::array<PlanetPatchId, 4> ChildrenOf(
    const PlanetPatchId id) noexcept
{
    const u8 next = static_cast<u8>(id.level + 1U);
    const u32 x = id.x * 2U;
    const u32 y = id.y * 2U;
    return {{
        {id.face, next, x + 0U, y + 0U},
        {id.face, next, x + 1U, y + 0U},
        {id.face, next, x + 0U, y + 1U},
        {id.face, next, x + 1U, y + 1U}
    }};
}

struct QueueEntry
{
    PlanetPatchId id{};
    f64 cellPixels{0.0};

    [[nodiscard]] bool operator<(
        const QueueEntry& other) const noexcept
    {
        return cellPixels < other.cellPixels;
    }
};

[[nodiscard]] u64 MeshFingerprint(
    const terrain::TerrainSource& source,
    const universe::BodyShape& shape,
    const PlanetPatchId id,
    const PlanetPatchMeshConfig& config)
{
    u64 value = 0x50415443484C4F44ULL;
    value = terrain::StableCombine64(value, source.Revision());
    value = terrain::StableCombine64(value, id.face);
    value = terrain::StableCombine64(value, id.level);
    value = terrain::StableCombine64(value, id.x);
    value = terrain::StableCombine64(value, id.y);
    value = terrain::StableCombine64(value, config.patchResolution);
    value = terrain::StableCombine64(value, std::bit_cast<u64>(config.footprintScale));
    value = terrain::StableCombine64(value, std::bit_cast<u64>(config.skirtDepthMeters));

    std::visit(
        [&value](const auto& bodyShape)
        {
            using Shape = std::decay_t<decltype(bodyShape)>;
            if constexpr (std::is_same_v<Shape, universe::SphereShape>)
            {
                value = terrain::StableCombine64(
                    value,
                    std::bit_cast<u64>(bodyShape.radiusMeters));
            }
            else
            {
                value = terrain::StableCombine64(
                    value,
                    std::bit_cast<u64>(bodyShape.radiiMeters.x));
                value = terrain::StableCombine64(
                    value,
                    std::bit_cast<u64>(bodyShape.radiiMeters.y));
                value = terrain::StableCombine64(
                    value,
                    std::bit_cast<u64>(bodyShape.radiiMeters.z));
            }
        },
        shape);

    return value;
}
} // namespace

PlanetPatchSelection PlanetPatchSelector::Select(
    const universe::BodyShape& shape,
    const PlanetPatchView& view,
    const PlanetPatchSelectorConfig& config)
{
    if (!ValidConfig(config) ||
        !std::isfinite(view.verticalFovRadians) ||
        view.verticalFovRadians <= 0.0 ||
        !std::isfinite(view.maximumDisplacementMeters) ||
        view.maximumDisplacementMeters < 0.0)
    {
        throw std::invalid_argument("Planet patch selector config/view is invalid.");
    }

    const f64 referenceRadius = ReferenceRadius(shape);
    if (!std::isfinite(referenceRadius) || referenceRadius <= 0.0)
    {
        throw std::invalid_argument("Planet patch selector body radius is invalid.");
    }

    PlanetPatchSelection result;
    std::map<PlanetPatchId, EvaluatedPatch> leaves;
    std::priority_queue<QueueEntry> queue;

    const auto evaluateAndInsert =
        [&](const PlanetPatchId id)
        {
            auto evaluated = EvaluatePatch(
                id,
                referenceRadius,
                view,
                config);
            ++result.stats.candidatesEvaluated;
            if (evaluated.horizonCulled)
            {
                ++result.stats.horizonCulledNodes;
            }
            if (evaluated.frustumCulled)
            {
                ++result.stats.frustumCulledNodes;
            }
            if (!evaluated.visible)
            {
                return;
            }

            const bool wasRefined = previouslyRefined_.contains(id);
            const f64 threshold = config.targetCellPixels *
                (wasRefined
                    ? (1.0 - config.hysteresisFraction)
                    : (1.0 + config.hysteresisFraction));

            leaves.insert_or_assign(id, evaluated);
            if (id.level < config.maximumLevel &&
                evaluated.cellPixels > threshold)
            {
                queue.push({id, evaluated.cellPixels});
            }
        };

    for (u8 face = 0U; face < 6U; ++face)
    {
        evaluateAndInsert({.face = face});
    }

    while (!queue.empty())
    {
        const auto entry = queue.top();
        queue.pop();

        const auto found = leaves.find(entry.id);
        if (found == leaves.end())
        {
            continue;
        }

        const bool wasRefined = previouslyRefined_.contains(entry.id);
        const f64 threshold = config.targetCellPixels *
            (wasRefined
                ? (1.0 - config.hysteresisFraction)
                : (1.0 + config.hysteresisFraction));

        if (found->second.cellPixels <= threshold ||
            entry.id.level >= config.maximumLevel)
        {
            continue;
        }

        // Replacing one visible parent can add at most four visible children,
        // i.e. a net +3 leaves. Stop before exceeding the hard frame budget.
        if (leaves.size() + 3U > config.maximumSelectedPatches)
        {
            result.stats.patchBudgetLimited = true;
            break;
        }

        leaves.erase(found);
        ++result.stats.refinedNodes;
        for (const auto child : ChildrenOf(entry.id))
        {
            evaluateAndInsert(child);
        }
    }

    result.patches.reserve(leaves.size());
    std::set<PlanetPatchId> refinedForNextFrame;
    for (const auto& [id, ignored] : leaves)
    {
        static_cast<void>(ignored);
        result.patches.push_back(id);
        result.stats.maximumSelectedLevel = std::max(
            result.stats.maximumSelectedLevel,
            static_cast<u32>(id.level));

        auto ancestor = id;
        while (ancestor.level > 0U)
        {
            ancestor = ParentOf(ancestor);
            refinedForNextFrame.insert(ancestor);
        }
    }

    result.stats.selectedPatches =
        static_cast<u32>(result.patches.size());
    previouslyRefined_ = std::move(refinedForNextFrame);
    return result;
}

void PlanetPatchSelector::Reset() noexcept
{
    previouslyRefined_.clear();
}

math::Double3 PlanetPatchDirection(
    const PlanetPatchId id,
    const f64 localU,
    const f64 localV)
{
    if (id.face >= 6U || id.level > 24U ||
        !std::isfinite(localU) || !std::isfinite(localV))
    {
        throw std::invalid_argument("Planet patch direction input is invalid.");
    }

    const u32 side = 1U << id.level;
    if (id.x >= side || id.y >= side)
    {
        throw std::out_of_range("Planet patch address is outside its level.");
    }

    const auto uv = UvBounds(id);
    const f64 u = uv.u0 + (uv.u1 - uv.u0) * std::clamp(localU, 0.0, 1.0);
    const f64 v = uv.v0 + (uv.v1 - uv.v0) * std::clamp(localV, 0.0, 1.0);
    return FaceDirection(id.face, u, v);
}

u64 PlanetPatchFingerprint(
    const terrain::TerrainSource& source,
    const universe::BodyShape& shape,
    const PlanetPatchId id,
    const PlanetPatchMeshConfig& config)
{
    if (id.face >= 6U || id.level > 24U ||
        config.patchResolution < 3U ||
        !std::isfinite(config.footprintScale) ||
        config.footprintScale <= 0.0 ||
        !std::isfinite(config.skirtDepthMeters) ||
        config.skirtDepthMeters < 0.0)
    {
        throw std::invalid_argument("Planet patch mesh config is invalid.");
    }

    const u32 side = 1U << id.level;
    if (id.x >= side || id.y >= side)
    {
        throw std::out_of_range("Planet patch address is outside its level.");
    }

    return MeshFingerprint(source, shape, id, config);
}

PlanetPatchMesh BuildPlanetPatch(
    const terrain::TerrainSource& source,
    const universe::BodyShape& shape,
    const PlanetPatchId id,
    const PlanetPatchMeshConfig& config)
{
    const u64 fingerprint = PlanetPatchFingerprint(source, shape, id, config);
    const f64 referenceRadius = ReferenceRadius(shape);
    if (!std::isfinite(referenceRadius) || referenceRadius <= 0.0)
    {
        throw std::invalid_argument("Planet patch body radius is invalid.");
    }

    const u32 n = config.patchResolution;
    const f64 angularCell =
        (0.5 * std::numbers::pi_v<f64>) /
        (static_cast<f64>(1ULL << id.level) * static_cast<f64>(n - 1U));

    PlanetPatchMesh result;
    result.id = id;
    result.referenceRadiusMeters = referenceRadius;
    result.sampleFootprintMeters =
        referenceRadius * angularCell * config.footprintScale;
    result.skirtDepthMeters =
        config.skirtDepthMeters > 0.0
            ? config.skirtDepthMeters
            : std::max(result.sampleFootprintMeters * 1.5, 1.0);
    result.sourceRevision = source.Revision();
    result.fingerprint = fingerprint;
    result.surfaceVertexCount = n * n;
    result.minimumRadiusMeters = std::numeric_limits<f64>::max();
    result.maximumRadiusMeters = 0.0;
    result.vertices.resize(static_cast<std::size_t>(n) * n);

    std::vector<u32> rows(n);
    std::iota(rows.begin(), rows.end(), 0U);
    std::vector<f64> rowMinimum(n, std::numeric_limits<f64>::max());
    std::vector<f64> rowMaximum(n, 0.0);

    std::for_each(
        std::execution::par,
        rows.begin(),
        rows.end(),
        [&](const u32 y)
        {
            for (u32 x = 0U; x < n; ++x)
            {
                const f64 localU =
                    static_cast<f64>(x) / static_cast<f64>(n - 1U);
                const f64 localV =
                    static_cast<f64>(y) / static_cast<f64>(n - 1U);
                const auto direction = PlanetPatchDirection(id, localU, localV);
                const auto sample = source.Sample({
                    .unitDirection = direction,
                    .footprintMeters = result.sampleFootprintMeters
                });
                const f64 radius =
                    RadiusAlong(shape, direction) +
                    sample.elevationMeters +
                    sample.standingWaterDepthMeters;
                const u32 index = y * n + x;
                result.vertices[index] = {
                    .positionMeters = direction * radius,
                    .normal = {},
                    .elevationMeters = sample.elevationMeters
                };
                rowMinimum[y] = std::min(rowMinimum[y], radius);
                rowMaximum[y] = std::max(rowMaximum[y], radius);
            }
        });

    for (u32 y = 0U; y < n; ++y)
    {
        result.minimumRadiusMeters = std::min(result.minimumRadiusMeters, rowMinimum[y]);
        result.maximumRadiusMeters = std::max(result.maximumRadiusMeters, rowMaximum[y]);
    }

    result.indices.reserve(
        static_cast<std::size_t>(n - 1U) * (n - 1U) * 6U +
        static_cast<std::size_t>(n - 1U) * 4U * 12U);

    for (u32 y = 0U; y + 1U < n; ++y)
    {
        for (u32 x = 0U; x + 1U < n; ++x)
        {
            const u32 i0 = y * n + x;
            const u32 i1 = i0 + 1U;
            const u32 i2 = i0 + n;
            const u32 i3 = i2 + 1U;
            const auto p0 = result.vertices[i0].positionMeters;
            const auto p1 = result.vertices[i1].positionMeters;
            const auto p2 = result.vertices[i2].positionMeters;
            const bool mathematicalOutward =
                math::Dot(math::Cross(p1 - p0, p2 - p0), p0) > 0.0;
            if (mathematicalOutward)
            {
                result.indices.insert(result.indices.end(), {i0, i2, i1, i1, i2, i3});
            }
            else
            {
                result.indices.insert(result.indices.end(), {i0, i1, i2, i1, i3, i2});
            }
        }
    }

    const f64 epsilon = std::max(angularCell * 0.35, 1.0e-7);
    const auto displacedPosition =
        [&](const math::Double3 direction)
        {
            const auto unit = math::Normalize(direction);
            const auto sample = source.Sample({
                .unitDirection = unit,
                .footprintMeters = result.sampleFootprintMeters
            });
            return unit *
                (RadiusAlong(shape, unit) +
                 sample.elevationMeters +
                 sample.standingWaterDepthMeters);
        };

    std::for_each(
        std::execution::par,
        result.vertices.begin(),
        result.vertices.end(),
        [&](PlanetPatchVertex& vertex)
        {
            const auto direction = math::Normalize(vertex.positionMeters);
            const math::Double3 reference =
                std::abs(direction.y) < 0.9
                    ? math::Double3{0.0, 1.0, 0.0}
                    : math::Double3{1.0, 0.0, 0.0};
            const auto tangentA = math::Normalize(math::Cross(reference, direction));
            const auto tangentB = math::Normalize(math::Cross(direction, tangentA));
            const auto aMinus = displacedPosition(direction - tangentA * epsilon);
            const auto aPlus = displacedPosition(direction + tangentA * epsilon);
            const auto bMinus = displacedPosition(direction - tangentB * epsilon);
            const auto bPlus = displacedPosition(direction + tangentB * epsilon);
            auto normal = math::Cross(aPlus - aMinus, bPlus - bMinus);
            if (math::Dot(normal, direction) < 0.0)
            {
                normal = normal * -1.0;
            }
            vertex.normal =
                math::LengthSquared(normal) > 1.0e-24
                    ? math::Normalize(normal)
                    : direction;
        });

    const auto appendSkirtSegment =
        [&](const u32 a, const u32 b)
        {
            const auto makeSkirtVertex =
                [&](const u32 surfaceIndex)
                {
                    auto vertex = result.vertices[surfaceIndex];
                    const auto direction = math::Normalize(vertex.positionMeters);
                    const f64 radius = math::Length(vertex.positionMeters);
                    vertex.positionMeters = direction *
                        std::max(radius - result.skirtDepthMeters, 1.0);
                    return vertex;
                };

            const u32 sa = static_cast<u32>(result.vertices.size());
            result.vertices.push_back(makeSkirtVertex(a));
            const u32 sb = static_cast<u32>(result.vertices.size());
            result.vertices.push_back(makeSkirtVertex(b));

            // Store both windings. Only the outward-facing winding survives
            // back-face culling; the duplicate makes the skirt independent of
            // cube-face/edge orientation and reliably seals T-junctions.
            result.indices.insert(
                result.indices.end(),
                {a, b, sb, a, sb, sa,
                 sb, b, a, sa, sb, a});
        };

    for (u32 x = 0U; x + 1U < n; ++x)
    {
        appendSkirtSegment(x, x + 1U);
        const u32 bottom = (n - 1U) * n;
        appendSkirtSegment(bottom + x + 1U, bottom + x);
    }
    for (u32 y = 0U; y + 1U < n; ++y)
    {
        appendSkirtSegment((y + 1U) * n, y * n);
        appendSkirtSegment(y * n + (n - 1U), (y + 1U) * n + (n - 1U));
    }

    return result;
}
} // namespace orbit::celestial_globe

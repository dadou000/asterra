#include <orbit/paths/Routing.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace orbit::paths
{
namespace
{
constexpr f64 kEpsilon = 1.0e-9;
constexpr std::size_t kMaximumGridCells = 500'000;

struct GridPoint
{
    i32 x{0};
    i32 y{0};
};

struct OpenNode
{
    f64 score{0.0};
    i32 index{-1};

    [[nodiscard]] bool operator<(
        const OpenNode& other) const noexcept
    {
        return score > other.score;
    }
};

[[nodiscard]] math::Double3 SafeUp(
    const math::Double3 requested,
    const math::Double3 forward) noexcept
{
    math::Double3 up =
        math::Normalize(requested);

    if (math::LengthSquared(up) <= kEpsilon)
    {
        up = {0.0, 1.0, 0.0};
    }

    if (std::abs(math::Dot(up, forward)) > 0.98)
    {
        up = std::abs(forward.y) < 0.98
            ? math::Double3{0.0, 1.0, 0.0}
            : math::Double3{1.0, 0.0, 0.0};
    }

    return up;
}

[[nodiscard]] f64 SegmentGrade(
    const math::Double3& a,
    const math::Double3& b,
    const math::Double3& up) noexcept
{
    const math::Double3 delta = b - a;
    const f64 vertical =
        std::abs(math::Dot(delta, up));
    const f64 lengthSquared =
        math::LengthSquared(delta);
    const f64 horizontal =
        std::sqrt(
            std::max(
                0.0,
                lengthSquared -
                    vertical * vertical));

    if (horizontal <= kEpsilon)
    {
        return vertical <= kEpsilon
            ? 0.0
            : std::numeric_limits<f64>::infinity();
    }

    return vertical / horizontal;
}

[[nodiscard]] f64 TurnRadius(
    const math::Double3& previous,
    const math::Double3& current,
    const math::Double3& next) noexcept
{
    const math::Double3 incoming =
        current - previous;
    const math::Double3 outgoing =
        next - current;
    const f64 incomingLength =
        math::Length(incoming);
    const f64 outgoingLength =
        math::Length(outgoing);

    if (incomingLength <= kEpsilon ||
        outgoingLength <= kEpsilon)
    {
        return std::numeric_limits<f64>::infinity();
    }

    const f64 cosine =
        std::clamp(
            math::Dot(incoming, outgoing) /
                (incomingLength * outgoingLength),
            -1.0,
            1.0);
    const f64 angle = std::acos(cosine);

    if (angle <= 1.0e-6)
    {
        return std::numeric_limits<f64>::infinity();
    }

    return std::min(incomingLength, outgoingLength) /
        angle;
}

[[nodiscard]] std::vector<math::Double3>
SimplifyPolyline(
    const std::vector<math::Double3>& input)
{
    if (input.size() <= 2)
    {
        return input;
    }

    std::vector<math::Double3> result;
    result.reserve(input.size());
    result.push_back(input.front());

    for (std::size_t index = 1;
         index + 1 < input.size();
         ++index)
    {
        const math::Double3 a =
            math::Normalize(
                input[index] -
                result.back());
        const math::Double3 b =
            math::Normalize(
                input[index + 1] -
                input[index]);

        if (math::LengthSquared(a) <= kEpsilon ||
            math::LengthSquared(b) <= kEpsilon ||
            math::Dot(a, b) < 0.9995)
        {
            result.push_back(input[index]);
        }
    }

    result.push_back(input.back());
    return result;
}

[[nodiscard]] f64 SamplePenalty(
    const RouteCostSample& sample,
    const PathProfile& profile) noexcept
{
    return
        std::max(0.0, sample.additiveCost) +
        std::max(0.0, sample.cutMeters) *
            profile.terrainCutCost +
        std::max(0.0, sample.fillMeters) *
            profile.terrainFillCost +
        std::max(0.0, sample.waterDepthMeters) *
            profile.waterCrossingCost;
}
} // namespace

std::optional<RouteProduct> SolveRoute(
    const RouteSolveRequest& request,
    std::string* failureReason)
{
    const auto fail =
        [failureReason](std::string message)
            -> std::optional<RouteProduct>
        {
            if (failureReason != nullptr)
            {
                *failureReason =
                    std::move(message);
            }
            return std::nullopt;
        };

    if (!request.costSource)
    {
        return fail("Route solve requires a cost source.");
    }

    if (!std::isfinite(request.cellSizeMeters) ||
        request.cellSizeMeters <= 0.0 ||
        !std::isfinite(request.corridorHalfWidthMeters) ||
        request.corridorHalfWidthMeters < 0.0)
    {
        return fail("Route grid dimensions are invalid.");
    }

    const math::Double3 direct =
        request.end - request.start;
    const f64 directLength =
        math::Length(direct);

    if (!std::isfinite(directLength) ||
        directLength <= kEpsilon)
    {
        return fail("Route endpoints must be distinct.");
    }

    const math::Double3 forward =
        direct / directLength;
    const math::Double3 up =
        SafeUp(request.up, forward);
    const math::Double3 lateral =
        math::Normalize(
            math::Cross(up, forward));

    if (math::LengthSquared(lateral) <= kEpsilon)
    {
        return fail("Route solver could not construct a corridor basis.");
    }

    const i32 columns =
        std::max<i32>(
            2,
            static_cast<i32>(
                std::ceil(
                    directLength /
                    request.cellSizeMeters)) +
                1);
    const i32 halfRows =
        static_cast<i32>(
            std::ceil(
                request.corridorHalfWidthMeters /
                request.cellSizeMeters));
    const i32 rows =
        halfRows * 2 + 1;

    const std::size_t cellCount =
        static_cast<std::size_t>(columns) *
        static_cast<std::size_t>(rows);

    if (cellCount == 0 ||
        cellCount > kMaximumGridCells)
    {
        return fail("Route corridor exceeds the CPU routing grid budget.");
    }

    const auto indexOf =
        [rows](const i32 x, const i32 y)
        {
            return x * rows + y;
        };

    const auto pointOf =
        [&](const i32 x, const i32 y)
        {
            const f64 longitudinal =
                static_cast<f64>(x) /
                static_cast<f64>(columns - 1);
            const f64 lateralMeters =
                static_cast<f64>(y - halfRows) *
                request.cellSizeMeters;

            return request.start +
                direct * longitudinal +
                lateral * lateralMeters;
        };

    std::vector<std::optional<RouteCostSample>>
        samples(cellCount);

    const auto sampleAt =
        [&](const i32 x, const i32 y)
            -> const RouteCostSample&
        {
            const i32 index = indexOf(x, y);
            auto& cached =
                samples[static_cast<std::size_t>(index)];

            if (!cached.has_value())
            {
                cached = request.costSource(
                    pointOf(x, y));
            }

            return *cached;
        };

    const i32 startIndex =
        indexOf(0, halfRows);
    const i32 goalIndex =
        indexOf(columns - 1, halfRows);

    const auto& startSample =
        sampleAt(0, halfRows);
    const auto& goalSample =
        sampleAt(columns - 1, halfRows);

    if (!startSample.traversable ||
        !goalSample.traversable)
    {
        return fail("A route endpoint is not traversable.");
    }

    std::vector<f64> costs(
        cellCount,
        std::numeric_limits<f64>::infinity());
    std::vector<i32> parents(cellCount, -1);
    std::vector<bool> closed(cellCount, false);
    std::priority_queue<OpenNode> open;

    costs[static_cast<std::size_t>(startIndex)] = 0.0;
    open.push({
        .score = directLength,
        .index = startIndex
    });

    constexpr i32 kDirections[8][2]{
        {1, 0}, {1, 1}, {1, -1},
        {0, 1}, {0, -1},
        {-1, 0}, {-1, 1}, {-1, -1}
    };

    while (!open.empty())
    {
        const i32 currentIndex =
            open.top().index;
        open.pop();

        if (closed[static_cast<std::size_t>(currentIndex)])
        {
            continue;
        }

        closed[static_cast<std::size_t>(currentIndex)] = true;

        if (currentIndex == goalIndex)
        {
            break;
        }

        const i32 currentX =
            currentIndex / rows;
        const i32 currentY =
            currentIndex % rows;
        const auto& currentSample =
            sampleAt(currentX, currentY);

        for (const auto& direction : kDirections)
        {
            const i32 nextX =
                currentX + direction[0];
            const i32 nextY =
                currentY + direction[1];

            if (nextX < 0 ||
                nextX >= columns ||
                nextY < 0 ||
                nextY >= rows)
            {
                continue;
            }

            const i32 nextIndex =
                indexOf(nextX, nextY);

            if (closed[static_cast<std::size_t>(nextIndex)])
            {
                continue;
            }

            const auto& nextSample =
                sampleAt(nextX, nextY);

            if (!nextSample.traversable ||
                (nextSample.waterDepthMeters > 0.0 &&
                 !request.profile.allowBridge))
            {
                continue;
            }

            const f64 grade =
                SegmentGrade(
                    currentSample.position,
                    nextSample.position,
                    up);

            if (!std::isfinite(grade) ||
                grade > request.profile.maximumGrade)
            {
                continue;
            }

            const i32 parentIndex =
                parents[static_cast<std::size_t>(currentIndex)];

            if (parentIndex >= 0 &&
                request.profile.minimumRadiusMeters > 0.0)
            {
                const i32 parentX =
                    parentIndex / rows;
                const i32 parentY =
                    parentIndex % rows;
                const auto& parentSample =
                    sampleAt(parentX, parentY);
                const f64 radius =
                    TurnRadius(
                        parentSample.position,
                        currentSample.position,
                        nextSample.position);

                if (radius <
                    request.profile.minimumRadiusMeters)
                {
                    continue;
                }
            }

            const f64 distance =
                math::Length(
                    nextSample.position -
                    currentSample.position);
            const f64 penalty =
                0.5 *
                (SamplePenalty(
                     currentSample,
                     request.profile) +
                 SamplePenalty(
                     nextSample,
                     request.profile));
            const f64 tentative =
                costs[static_cast<std::size_t>(currentIndex)] +
                distance * (1.0 + penalty);

            if (tentative >=
                costs[static_cast<std::size_t>(nextIndex)])
            {
                continue;
            }

            parents[static_cast<std::size_t>(nextIndex)] =
                currentIndex;
            costs[static_cast<std::size_t>(nextIndex)] =
                tentative;

            const f64 heuristic =
                math::Length(
                    goalSample.position -
                    nextSample.position);

            open.push({
                .score = tentative + heuristic,
                .index = nextIndex
            });
        }
    }

    if (!std::isfinite(
            costs[static_cast<std::size_t>(goalIndex)]))
    {
        return fail("No route satisfies the active path profile and cost field.");
    }

    std::vector<math::Double3> reversed;
    i32 cursor = goalIndex;

    while (cursor >= 0)
    {
        const i32 x = cursor / rows;
        const i32 y = cursor % rows;
        reversed.push_back(
            sampleAt(x, y).position);

        if (cursor == startIndex)
        {
            break;
        }

        cursor =
            parents[static_cast<std::size_t>(cursor)];
    }

    if (reversed.empty() ||
        cursor != startIndex)
    {
        return fail("Route reconstruction failed.");
    }

    std::reverse(
        reversed.begin(),
        reversed.end());

    std::vector<math::Double3> points =
        SimplifyPolyline(reversed);

    f64 length = 0.0;
    f64 maximumGrade = 0.0;

    for (std::size_t index = 1;
         index < points.size();
         ++index)
    {
        length +=
            math::Length(
                points[index] -
                points[index - 1]);
        maximumGrade =
            std::max(
                maximumGrade,
                SegmentGrade(
                    points[index - 1],
                    points[index],
                    up));
    }

    return RouteProduct{
        .points = std::move(points),
        .totalCost =
            costs[static_cast<std::size_t>(goalIndex)],
        .lengthMeters = length,
        .maximumGrade = maximumGrade,
        .dependencyRevision =
            request.dependencyRevision,
        .routeRevision = 0
    };
}

class RoutingService::Impl
{
public:
    struct PendingResult
    {
        mutable std::mutex mutex;
        bool complete{false};
        u64 generation{0};
        std::optional<RouteProduct> product;
        std::string error;
    };

    struct Entry
    {
        RouteSolveRequest request;
        RouteStatus status;
        std::optional<RouteProduct> product;
        std::shared_ptr<PendingResult> pending;
    };

    explicit Impl(jobs::JobSystem& jobSystem)
        : jobs(jobSystem)
    {
    }

    ~Impl()
    {
        jobs.Wait(group);
    }

    void Submit(
        const scene::ObjectId edge,
        Entry& entry)
    {
        entry.status.state =
            RouteBuildState::Building;
        const u64 generation =
            entry.status.requestedGeneration;
        RouteSolveRequest request =
            entry.request;
        auto pending =
            std::make_shared<PendingResult>();
        pending->generation = generation;
        entry.pending = pending;

        jobs.Submit(
            group,
            jobs::JobPriority::Normal,
            [pending,
             request = std::move(request),
             edge]() mutable
            {
                static_cast<void>(edge);
                std::string error;
                auto product =
                    SolveRoute(
                        request,
                        &error);

                std::scoped_lock lock(
                    pending->mutex);
                pending->product =
                    std::move(product);
                pending->error =
                    std::move(error);
                pending->complete = true;
            });
    }

    jobs::JobSystem& jobs;
    jobs::JobGroup group;
    std::unordered_map<scene::ObjectId, Entry>
        entries;
    u64 nextRouteRevision{1};
};

RoutingService::RoutingService(
    jobs::JobSystem& jobs)
    : impl_(
          std::make_unique<Impl>(jobs))
{
}

RoutingService::~RoutingService() = default;

void RoutingService::Request(
    const scene::ObjectId edge,
    RouteSolveRequest request)
{
    if (!edge)
    {
        throw std::invalid_argument(
            "RoutingService requires a valid edge ID.");
    }

    auto& entry = impl_->entries[edge];
    entry.request = std::move(request);
    ++entry.status.requestedGeneration;
    entry.status.dependencyRevision =
        entry.request.dependencyRevision;
    entry.status.error.clear();
    entry.status.state =
        RouteBuildState::Queued;
    entry.pending.reset();
}

void RoutingService::Invalidate(
    const scene::ObjectId edge,
    const u64 dependencyRevision)
{
    const auto found =
        impl_->entries.find(edge);

    if (found == impl_->entries.end())
    {
        return;
    }

    auto& entry = found->second;

    if (entry.status.dependencyRevision ==
            dependencyRevision &&
        entry.status.state ==
            RouteBuildState::Ready)
    {
        return;
    }

    entry.request.dependencyRevision =
        dependencyRevision;
    entry.status.dependencyRevision =
        dependencyRevision;
    ++entry.status.requestedGeneration;
    entry.status.state =
        RouteBuildState::Dirty;
    entry.status.error.clear();
    entry.pending.reset();
}

void RoutingService::Remove(
    const scene::ObjectId edge)
{
    impl_->entries.erase(edge);
}

void RoutingService::Poll()
{
    for (auto& [edge, entry] :
         impl_->entries)
    {
        if (entry.status.state ==
                RouteBuildState::Queued ||
            entry.status.state ==
                RouteBuildState::Dirty)
        {
            impl_->Submit(edge, entry);
            continue;
        }

        if (entry.status.state !=
                RouteBuildState::Building ||
            !entry.pending)
        {
            continue;
        }

        std::scoped_lock lock(
            entry.pending->mutex);

        if (!entry.pending->complete)
        {
            continue;
        }

        if (entry.pending->generation !=
            entry.status.requestedGeneration)
        {
            entry.pending.reset();
            entry.status.state =
                RouteBuildState::Dirty;
            continue;
        }

        if (!entry.pending->product.has_value())
        {
            entry.product.reset();
            entry.status.error =
                entry.pending->error;
            entry.status.state =
                RouteBuildState::Failed;
            entry.pending.reset();
            continue;
        }

        RouteProduct product =
            std::move(
                *entry.pending->product);
        product.routeRevision =
            impl_->nextRouteRevision++;
        entry.status.routeRevision =
            product.routeRevision;
        entry.status.committedDependencyRevision =
            product.dependencyRevision;
        entry.status.error.clear();
        entry.product =
            std::move(product);
        entry.status.state =
            RouteBuildState::Ready;
        entry.pending.reset();
    }
}

RouteStatus RoutingService::Status(
    const scene::ObjectId edge) const
{
    const auto found =
        impl_->entries.find(edge);

    return found == impl_->entries.end()
        ? RouteStatus{}
        : found->second.status;
}

const RouteProduct* RoutingService::Product(
    const scene::ObjectId edge) const noexcept
{
    const auto found =
        impl_->entries.find(edge);

    if (found == impl_->entries.end() ||
        !found->second.product.has_value())
    {
        return nullptr;
    }

    return &*found->second.product;
}
} // namespace orbit::paths

#include <orbit/terrain_material_column/Local3DPromotion.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace orbit::terrain_material_column
{
namespace
{
[[nodiscard]] bool FiniteNonNegative(
    const f64 value) noexcept
{
    return std::isfinite(value) && value >= 0.0;
}

[[nodiscard]] bool FinitePositive(
    const f64 value) noexcept
{
    return std::isfinite(value) && value > 0.0;
}

[[nodiscard]] bool FiniteUnit(
    const f32 value) noexcept
{
    return std::isfinite(value) &&
           value >= 0.0F &&
           value <= 1.0F;
}

[[nodiscard]] bool ValidTileAddress(
    const terrain::PhysicalTerrainPageAddress& address) noexcept
{
    return address.planet.IsValid();
}

[[nodiscard]] u64 MixF64(
    u64 value,
    const f64 field) noexcept
{
    return terrain::StableCombine64(
        value,
        std::bit_cast<u64>(field));
}

[[nodiscard]] u64 MixF32(
    u64 value,
    const f32 field) noexcept
{
    return terrain::StableCombine64(
        value,
        static_cast<u64>(std::bit_cast<u32>(field)));
}
} // namespace

bool Local3DPromotionPolicy::IsValid() const noexcept
{
    return
        FiniteNonNegative(minimumVoidHeightMeters) &&
        FiniteNonNegative(minimumUndercutMeters) &&
        FiniteNonNegative(minimumPersistenceMeters) &&
        FiniteUnit(minimumProcessConfidence) &&
        boundaryGuardCells > 0 &&
        FinitePositive(belowSurfaceWindowMeters) &&
        FinitePositive(aboveSurfaceWindowMeters) &&
        maximumSolidSpansPerCell >= 1;
}

bool Local3DPromotionRequest::IsValid() const noexcept
{
    return
        ValidTileAddress(address) &&
        FiniteNonNegative(voidHeightMeters) &&
        FiniteNonNegative(undercutMeters) &&
        FiniteNonNegative(persistenceMeters) &&
        FiniteUnit(processConfidence);
}

bool ShouldPromoteLocal3D(
    const Local3DPromotionRequest& request,
    const Local3DPromotionPolicy& policy) noexcept
{
    if (!request.IsValid() || !policy.IsValid())
    {
        return false;
    }

    if (request.reason ==
        Local3DPromotionReason::ExplicitAuthoring)
    {
        return true;
    }

    if (!policy.allowProcessTriggered)
    {
        return false;
    }

    return
        request.voidHeightMeters >=
            policy.minimumVoidHeightMeters &&
        request.undercutMeters >=
            policy.minimumUndercutMeters &&
        request.persistenceMeters >=
            policy.minimumPersistenceMeters &&
        request.processConfidence >=
            policy.minimumProcessConfidence;
}

u64 Local3DPromotionFingerprint(
    const Local3DPromotionRequest& request,
    const Local3DPromotionPolicy& policy) noexcept
{
    if (!request.IsValid() || !policy.IsValid())
    {
        return 0;
    }

    u64 value = 0x4D32344C33445052ULL; // "M24L3DPR"
    value = terrain::StableCombine64(
        value,
        request.address.planet.high);
    value = terrain::StableCombine64(
        value,
        request.address.planet.low);
    value = terrain::StableCombine64(
        value,
        static_cast<u64>(request.address.tile.face));
    value = terrain::StableCombine64(
        value,
        request.address.tile.level);
    value = terrain::StableCombine64(
        value,
        request.address.tile.x);
    value = terrain::StableCombine64(
        value,
        request.address.tile.y);
    value = terrain::StableCombine64(
        value,
        static_cast<u64>(request.reason));
    value = terrain::StableCombine64(
        value,
        request.sourceRevision);
    value = terrain::StableCombine64(
        value,
        request.promotionRevision);

    value = MixF64(value, request.voidHeightMeters);
    value = MixF64(value, request.undercutMeters);
    value = MixF64(value, request.persistenceMeters);
    value = MixF32(value, request.processConfidence);

    value = terrain::StableCombine64(
        value,
        policy.allowProcessTriggered ? 1ULL : 0ULL);
    value = MixF64(value, policy.minimumVoidHeightMeters);
    value = MixF64(value, policy.minimumUndercutMeters);
    value = MixF64(value, policy.minimumPersistenceMeters);
    value = MixF32(value, policy.minimumProcessConfidence);
    value = terrain::StableCombine64(
        value,
        policy.boundaryGuardCells);
    value = MixF64(
        value,
        policy.belowSurfaceWindowMeters);
    value = MixF64(
        value,
        policy.aboveSurfaceWindowMeters);
    value = terrain::StableCombine64(
        value,
        policy.maximumSolidSpansPerCell);

    return value;
}

bool Local3DSolidSpan::IsValid() const noexcept
{
    return
        std::isfinite(bottomHeightMeters) &&
        std::isfinite(topHeightMeters) &&
        topHeightMeters > bottomHeightMeters &&
        material.IsValid();
}

bool Local3DCell::IsValid(
    const u32 maximumSolidSpans) const noexcept
{
    if (solidSpans.size() >
        static_cast<std::size_t>(maximumSolidSpans))
    {
        return false;
    }

    f32 previousTop =
        -std::numeric_limits<f32>::infinity();

    for (const auto& span : solidSpans)
    {
        if (!span.IsValid() ||
            span.bottomHeightMeters < previousTop)
        {
            return false;
        }

        previousTop = span.topHeightMeters;
    }

    return true;
}

bool Local3DCell::ContainsSolid(
    const f32 heightMeters) const noexcept
{
    if (!std::isfinite(heightMeters))
    {
        return false;
    }

    for (const auto& span : solidSpans)
    {
        if (heightMeters >= span.bottomHeightMeters &&
            heightMeters <= span.topHeightMeters)
        {
            return true;
        }
    }

    return false;
}

std::optional<f32>
Local3DCell::TopSurfaceHeightMeters() const noexcept
{
    if (solidSpans.empty())
    {
        return std::nullopt;
    }

    return solidSpans.back().topHeightMeters;
}

Local3DPromotionRegion::Local3DPromotionRegion(
    const MaterialColumnPage& source,
    Local3DPromotionRequest request,
    Local3DPromotionPolicy policy)
    : resolution_(source.Resolution()),
      spacingMeters_(source.SpacingMeters()),
      request_(std::move(request)),
      policy_(std::move(policy)),
      fingerprint_(
          Local3DPromotionFingerprint(
              request_,
              policy_))
{
    if (!ShouldPromoteLocal3D(request_, policy_))
    {
        throw std::invalid_argument(
            "M24 Local3DPromotionRegion requires an accepted promotion request.");
    }

    if (resolution_ == 0 ||
        policy_.boundaryGuardCells * 2U >= resolution_)
    {
        throw std::invalid_argument(
            "M24 local 3D boundary guard consumes the whole source page.");
    }

    const std::size_t count =
        static_cast<std::size_t>(resolution_) *
        static_cast<std::size_t>(resolution_);

    baselineSurfaceHeights_.resize(count);
    cells_.resize(count);

    for (u32 y = 0; y < resolution_; ++y)
    {
        for (u32 x = 0; x < resolution_; ++x)
        {
            const std::size_t index = Index(x, y);
            const auto& sourceCell = source.At(x, y);

            if (!sourceCell.IsValid())
            {
                throw std::invalid_argument(
                    "M24 promotion source contains an invalid M08 cell.");
            }

            const f32 surface =
                sourceCell.SurfaceHeightMeters();

            baselineSurfaceHeights_[index] = surface;

            cells_[index].solidSpans.push_back({
                .bottomHeightMeters =
                    static_cast<f32>(
                        static_cast<f64>(surface) -
                        policy_.belowSurfaceWindowMeters),
                .topHeightMeters = surface,
                .material = sourceCell.bedrockMaterial
            });
        }
    }
}

u32 Local3DPromotionRegion::Resolution() const noexcept
{
    return resolution_;
}

f64 Local3DPromotionRegion::SpacingMeters() const noexcept
{
    return spacingMeters_;
}

const Local3DPromotionRequest&
Local3DPromotionRegion::Request() const noexcept
{
    return request_;
}

const Local3DPromotionPolicy&
Local3DPromotionRegion::Policy() const noexcept
{
    return policy_;
}

u64 Local3DPromotionRegion::Fingerprint() const noexcept
{
    return fingerprint_;
}

std::size_t Local3DPromotionRegion::Index(
    const u32 x,
    const u32 y) const
{
    if (x >= resolution_ || y >= resolution_)
    {
        throw std::out_of_range(
            "M24 local 3D coordinate is outside the promoted region.");
    }

    return
        static_cast<std::size_t>(y) *
            static_cast<std::size_t>(resolution_) +
        static_cast<std::size_t>(x);
}

bool Local3DPromotionRegion::IsBoundaryGuardCell(
    const u32 x,
    const u32 y) const
{
    (void)Index(x, y);

    const u32 guard = policy_.boundaryGuardCells;

    return
        x < guard ||
        y < guard ||
        x >= resolution_ - guard ||
        y >= resolution_ - guard;
}

Local3DCell& Local3DPromotionRegion::At(
    const u32 x,
    const u32 y)
{
    return cells_[Index(x, y)];
}

const Local3DCell& Local3DPromotionRegion::At(
    const u32 x,
    const u32 y) const
{
    return cells_[Index(x, y)];
}

bool Local3DPromotionRegion::IntervalInsideWindow(
    const std::size_t index,
    const f32 bottomHeightMeters,
    const f32 topHeightMeters) const noexcept
{
    if (!std::isfinite(bottomHeightMeters) ||
        !std::isfinite(topHeightMeters) ||
        topHeightMeters <= bottomHeightMeters)
    {
        return false;
    }

    const f64 surface =
        static_cast<f64>(
            baselineSurfaceHeights_[index]);

    const f64 minimum =
        surface - policy_.belowSurfaceWindowMeters;
    const f64 maximum =
        surface + policy_.aboveSurfaceWindowMeters;

    return
        static_cast<f64>(bottomHeightMeters) >= minimum &&
        static_cast<f64>(topHeightMeters) <= maximum;
}

bool Local3DPromotionRegion::CarveVoid(
    const u32 x,
    const u32 y,
    const f32 bottomHeightMeters,
    const f32 topHeightMeters)
{
    const std::size_t index = Index(x, y);

    if (IsBoundaryGuardCell(x, y) ||
        !IntervalInsideWindow(
            index,
            bottomHeightMeters,
            topHeightMeters))
    {
        return false;
    }

    const auto& current = cells_[index].solidSpans;
    std::vector<Local3DSolidSpan> carved;
    carved.reserve(current.size() + 1U);

    bool changed = false;

    for (const auto& span : current)
    {
        if (topHeightMeters <=
                span.bottomHeightMeters ||
            bottomHeightMeters >=
                span.topHeightMeters)
        {
            carved.push_back(span);
            continue;
        }

        changed = true;

        if (bottomHeightMeters >
            span.bottomHeightMeters)
        {
            carved.push_back({
                .bottomHeightMeters =
                    span.bottomHeightMeters,
                .topHeightMeters =
                    std::min(
                        bottomHeightMeters,
                        span.topHeightMeters),
                .material = span.material
            });
        }

        if (topHeightMeters <
            span.topHeightMeters)
        {
            carved.push_back({
                .bottomHeightMeters =
                    std::max(
                        topHeightMeters,
                        span.bottomHeightMeters),
                .topHeightMeters =
                    span.topHeightMeters,
                .material = span.material
            });
        }
    }

    if (!changed)
    {
        return false;
    }

    Local3DCell candidate{
        .solidSpans = std::move(carved)
    };

    if (!candidate.IsValid(
            policy_.maximumSolidSpansPerCell))
    {
        return false;
    }

    cells_[index] = std::move(candidate);
    return true;
}

bool Local3DPromotionRegion::AddSolid(
    const u32 x,
    const u32 y,
    Local3DSolidSpan span)
{
    const std::size_t index = Index(x, y);

    if (IsBoundaryGuardCell(x, y) ||
        !span.IsValid() ||
        !IntervalInsideWindow(
            index,
            span.bottomHeightMeters,
            span.topHeightMeters))
    {
        return false;
    }

    auto candidate = cells_[index].solidSpans;
    candidate.push_back(std::move(span));

    std::sort(
        candidate.begin(),
        candidate.end(),
        [](const Local3DSolidSpan& a,
           const Local3DSolidSpan& b)
        {
            return a.bottomHeightMeters <
                   b.bottomHeightMeters;
        });

    std::vector<Local3DSolidSpan> normalized;
    normalized.reserve(candidate.size());

    for (const auto& current : candidate)
    {
        if (normalized.empty())
        {
            normalized.push_back(current);
            continue;
        }

        auto& previous = normalized.back();

        if (current.bottomHeightMeters >
            previous.topHeightMeters)
        {
            normalized.push_back(current);
            continue;
        }

        if (current.material != previous.material)
        {
            return false;
        }

        previous.topHeightMeters =
            std::max(
                previous.topHeightMeters,
                current.topHeightMeters);
    }

    Local3DCell result{
        .solidSpans = std::move(normalized)
    };

    if (!result.IsValid(
            policy_.maximumSolidSpansPerCell))
    {
        return false;
    }

    cells_[index] = std::move(result);
    return true;
}

bool Local3DPromotionRegion::BoundaryMatches(
    const MaterialColumnPage& source,
    const f64 toleranceMeters) const
{
    if (source.Resolution() != resolution_ ||
        source.SpacingMeters() != spacingMeters_ ||
        !FiniteNonNegative(toleranceMeters))
    {
        return false;
    }

    for (u32 y = 0; y < resolution_; ++y)
    {
        for (u32 x = 0; x < resolution_; ++x)
        {
            if (!IsBoundaryGuardCell(x, y))
            {
                continue;
            }

            const auto& cell = At(x, y);
            if (cell.solidSpans.size() != 1U)
            {
                return false;
            }

            const f64 expected =
                source.At(x, y).SurfaceHeightMeters();
            const f64 actual =
                cell.solidSpans.front().
                    topHeightMeters;

            if (std::abs(expected - actual) >
                toleranceMeters)
            {
                return false;
            }
        }
    }

    return true;
}

std::size_t
Local3DPromotionRegion::ApproximateOwnedBytes() const noexcept
{
    std::size_t bytes =
        baselineSurfaceHeights_.capacity() *
            sizeof(f32) +
        cells_.capacity() *
            sizeof(Local3DCell);

    for (const auto& cell : cells_)
    {
        bytes +=
            cell.solidSpans.capacity() *
            sizeof(Local3DSolidSpan);
    }

    return bytes;
}

std::unique_ptr<Local3DPromotionRegion>
TryPromoteLocal3D(
    const MaterialColumnPage& source,
    const Local3DPromotionRequest& request,
    const Local3DPromotionPolicy& policy)
{
    if (!ShouldPromoteLocal3D(request, policy))
    {
        return nullptr;
    }

    return std::make_unique<Local3DPromotionRegion>(
        source,
        request,
        policy);
}
} // namespace orbit::terrain_material_column

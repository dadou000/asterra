#pragma once

#include "TectonicField.hpp"

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>

#include <atomic>
#include <memory>
#include <vector>

namespace orbit::terrain
{
// Bake-time plate ownership by noise-metric growth. Every plate grows from its
// seed over the cube-sphere raster with a Dijkstra whose step cost is the
// angular step length times a lognormal noise multiplier, so fronts stall
// against costly stretches and boundaries follow them: organic, non-convex,
// always connected (a plate is the set of cells it reaches first), and with
// cross-face growth through the cube edges. This is "random-order flood fill"
// generalised to a continuous cost.
//
// The per-plate arrival costs become the plate claims the boundary logic in
// TectonicField already works with (claim = base - scale * cost), and the
// boundary normal comes from the raster gradient instead of the seed
// directions. Costs are kept only within a boundary band of the winner, which
// is all the structure evaluation ever reads.
class TectonicGrowth
{
public:
    // Returns null if `cancel` was raised.
    [[nodiscard]] static std::shared_ptr<const TectonicGrowth> Build(
        const detail::TectonicField& field,
        u32 resolution,
        u64 seed,
        const std::atomic<bool>* cancel,
        u32 workers);

    [[nodiscard]] u32 Resolution() const noexcept { return resolution_; }

    // Claims of every plate at a raster texel, x and y in [-2, resolution + 1]
    // (the gutter and its neighbours resolve through the cube edges).
    void Claims(
        u32 face,
        i32 x,
        i32 y,
        detail::ClaimArray& claims) const noexcept;

    // Largest boundary-structure width (claim units) a plate can carry: about
    // 0.4 of how far it reaches from its seed, so small plates keep an interior.
    [[nodiscard]] const detail::ClaimArray& WidthLimits() const noexcept { return widthLimit_; }
    // Per-plate inverse growth rate: a plate's claims change with distance
    // inversely to its growth rate, so a boundary of fixed physical width is
    // a claim-difference width proportional to the mean of the two plates'.
    [[nodiscard]] const detail::ClaimArray& WidthScales() const noexcept { return widthScale_; }

    // Unit tangent at the texel pointing from plate `i` towards plate `j`,
    // from the gradient of the claim difference; zero if undefined.
    [[nodiscard]] math::Double3 BoundaryNormal(
        u32 face,
        i32 x,
        i32 y,
        u32 i,
        u32 j) const noexcept;

private:
    TectonicGrowth() = default;

    [[nodiscard]] std::size_t NodeAt(u32 face, i32 x, i32 y) const noexcept;
    [[nodiscard]] f64 ClaimAt(u32 plate, std::size_t node) const noexcept;
    // Claim at a raster position that may lie outside the face: inside it is
    // the texel, outside it is interpolated bilinearly on the face that owns
    // that direction (so the gutter is continuous across the cube edge).
    [[nodiscard]] f64 ClaimInterpolated(u32 plate, u32 face, i32 x, i32 y) const noexcept;

    u32 resolution_{0};
    u32 plateCount_{0};
    f64 claimBase_{1.0};
    // plateCount_ * (6 * resolution^2) arrival costs, blurred; a plate carries
    // its band-edge value outside its band.
    std::vector<f32> cost_;
    detail::ClaimArray widthLimit_{};
    detail::ClaimArray widthScale_{};
};
} // namespace orbit::terrain

#pragma once

#include <orbit/weather_lab/FastStormSolver.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Phase 2 of the storm domain plan (docs: /world/weather-lab/global-system-plan): a fine child grid inside the coarse parent.
//
// The child is a second FastStormSolver at dx / ratio over `cells` x `cells` parent cells, in the same Galilean frame and with the
// same vertical grid. It keeps the periodic grid and the exact FFT pressure projection; instead of an open boundary, a band of
// `relaxCells` child cells around its edge is relaxed toward the parent's state (interpolated in space and time) every step, before
// the projection. The parent runs on its own, so this is one-way nesting; `feedback` additionally nudges the parent toward the
// child's block averages inside the child's interior (two-way).
namespace orbit::weather_lab
{
struct NestConfig
{
    FastStormConfig parent{};
    std::uint32_t ratio = 4;         // parent dx / child dx: 2 or 4 (the child grid must stay a power of two)
    std::uint32_t cells = 32;        // child extent in parent cells; cells * ratio must be a power of two
    std::uint32_t originI = 0xFFFFFFFFU;  // child lower-left in parent cells; default centres it
    std::uint32_t originJ = 0xFFFFFFFFU;
    std::uint32_t relaxCells = 12;   // width of the relaxation band, in child cells
    float relaxSeconds = 10.0F;      // relaxation time at the outer edge
    bool feedback = false;           // two-way: child averages back into the parent interior
    float feedbackSeconds = 120.0F;  // nudging time of the parent toward the child average

    // Adaptive child: it is created (centred on the main updraft) when the parent's peak updraft reaches `createUpdraft` and
    // released after the peak stays below `releaseUpdraft` for `releaseHoldSeconds` (hysteresis). `maxChildCells` caps the child's
    // 3-D cell count: when `cells` would exceed it the child is made smaller instead (coarsening, never a longer step); 0 = no cap.
    bool adaptive = false;
    float createUpdraft = 8.0F;
    float releaseUpdraft = 3.0F;
    float releaseHoldSeconds = 900.0F;
    std::uint32_t maxChildCells = 0;
};

class NestedStormSolver
{
public:
    explicit NestedStormSolver(const NestConfig& config);
    ~NestedStormSolver();
    NestedStormSolver(const NestedStormSolver&) = delete;
    NestedStormSolver& operator=(const NestedStormSolver&) = delete;

    [[nodiscard]] static std::string Validate(const NestConfig& config);

    // Advances parent and child by `seconds` of simulated time (parent steps, child sub-steps to the parent's time).
    void Advance(double seconds);

    [[nodiscard]] FastStormSolver& Parent();
    // The child exists from the start unless `adaptive`; Child() requires HasChild().
    [[nodiscard]] bool HasChild() const;
    [[nodiscard]] FastStormSolver& Child();
    // Current child extent in parent cells (0 without a child), how many times a child was created, and how often the budget
    // made it smaller than `cells`.
    [[nodiscard]] std::uint32_t ChildCells() const;
    [[nodiscard]] std::uint32_t ChildCreations() const;
    [[nodiscard]] std::uint32_t BudgetCoarsenings() const;
    // Cells in the parent plus the child (the number the cost scales with).
    [[nodiscard]] std::uint64_t TotalCells() const;
    [[nodiscard]] const NestConfig& Config() const;
    // Child steps taken per parent step on average, and the share of the child's cells that are relaxed (not free).
    [[nodiscard]] double SubStepRatio() const;
    [[nodiscard]] double RelaxedFraction() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::weather_lab

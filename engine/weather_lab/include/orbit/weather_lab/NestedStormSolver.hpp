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
    [[nodiscard]] FastStormSolver& Child();
    [[nodiscard]] const NestConfig& Config() const;
    // Child steps taken per parent step on average, and the share of the child's cells that are relaxed (not free).
    [[nodiscard]] double SubStepRatio() const;
    [[nodiscard]] double RelaxedFraction() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::weather_lab

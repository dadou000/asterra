#include <orbit/weather_lab/NestedStormSolver.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace orbit::weather_lab
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr int kFields = 10;   // u v w theta qv qc qr qi qs qg

// Position of each field's samples inside a cell, in cells (the staggering of FastStormSolver).
struct Stagger
{
    float x;
    float y;
};
constexpr std::array<Stagger, kFields> kStagger{{
    {0.0F, 0.5F}, {0.5F, 0.0F}, {0.5F, 0.5F}, {0.5F, 0.5F}, {0.5F, 0.5F},
    {0.5F, 0.5F}, {0.5F, 0.5F}, {0.5F, 0.5F}, {0.5F, 0.5F}, {0.5F, 0.5F}}};

std::array<float*, kFields> Pointers(const FastStormState& s)
{
    return {s.u, s.v, s.w, s.theta, s.qv, s.qc, s.qr, s.qi, s.qs, s.qg};
}

std::size_t FieldSize(const int field, const FastStormState& s)
{
    return static_cast<std::size_t>(s.nx) * s.ny * (field == 2 ? s.nz + 1U : s.nz);
}

bool IsPowerOfTwo(const std::uint32_t v)
{
    return v >= 2U && (v & (v - 1U)) == 0U;
}
} // namespace

struct NestedStormSolver::Impl
{
    NestConfig cfg;
    std::unique_ptr<FastStormSolver> parent;
    std::unique_ptr<FastStormSolver> child;
    std::uint32_t cells = 0;       // current child extent in parent cells
    std::uint32_t originI = 0;
    std::uint32_t originJ = 0;
    std::uint32_t childNx = 0;
    std::uint32_t childNy = 0;
    bool ice = false;

    // Parent state at the start and the end of the current parent step.
    std::array<std::vector<float>, kFields> before;
    std::array<std::vector<float>, kFields> after;
    double t0 = 0.0;
    double t1 = 1.0;
    std::uint64_t parentSteps = 0;
    std::uint64_t childSteps = 0;
    std::uint32_t creations = 0;
    std::uint32_t coarsenings = 0;
    float lowTimer = 0.0F;

    explicit Impl(const NestConfig& config) : cfg(config)
    {
        ice = cfg.parent.ice;
        parent = std::make_unique<FastStormSolver>(cfg.parent);
        Capture(before);
        after = before;
        if (!cfg.adaptive)
        {
            const FastStormConfig& p = cfg.parent;
            const std::uint32_t oi = cfg.originI != 0xFFFFFFFFU ? cfg.originI : (p.nx - cfg.cells) / 2U;
            const std::uint32_t oj = cfg.originJ != 0xFFFFFFFFU ? cfg.originJ : (p.ny - cfg.cells) / 2U;
            CreateChild(cfg.cells, oi, oj);
        }
    }

    void CreateChild(const std::uint32_t wantedCells, const std::uint32_t oi, const std::uint32_t oj)
    {
        const FastStormConfig& p = cfg.parent;
        cells = wantedCells;
        // Budget: shrink the child (halving its extent) until its 3-D cell count fits.
        while (cfg.maxChildCells > 0U
            && static_cast<std::uint64_t>(cells) * cfg.ratio * cells * cfg.ratio * p.nz > cfg.maxChildCells
            && cells * cfg.ratio > 16U)
        {
            cells /= 2U;
        }
        if (cells < wantedCells)
        {
            ++coarsenings;
        }
        childNx = cells * cfg.ratio;
        childNy = cells * cfg.ratio;
        originI = std::clamp(oi, 2U, p.nx - cells - 2U);
        originJ = std::clamp(oj, 2U, p.ny - cells - 2U);

        FastStormConfig c = p;
        c.nx = childNx;
        c.ny = childNy;
        c.dx = p.dx / static_cast<float>(cfg.ratio);
        c.dy = p.dy / static_cast<float>(cfg.ratio);
        c.trackStorm = false;            // the child follows the parent's frame
        c.lateralSpongeCells = 0U;       // its edge is the relaxation band
        child = std::make_unique<FastStormSolver>(c);
        child->SetFrame(parent->Config().frameU, parent->Config().frameV);
        child->SetTime(parent->Diagnostics().time);
        // The parent snapshots must describe the current parent state for the initial fill.
        Capture(before);
        after = before;
        FillChildFromParent();
        child->MarkInitialState();
        child->SetPreProjectionHook(
            [this](const FastStormState& state, const double endTime, const float dt) { Relax(state, endTime, dt); });
        ++creations;
        lowTimer = 0.0F;
    }

    void Capture(std::array<std::vector<float>, kFields>& into)
    {
        const FastStormState s = parent->MutableState();
        const auto pointers = Pointers(s);
        for (int f = 0; f < kFields; ++f)
        {
            if (pointers[f] == nullptr || (f >= 7 && !ice))
            {
                into[static_cast<std::size_t>(f)].clear();
                continue;
            }
            into[static_cast<std::size_t>(f)].assign(pointers[f], pointers[f] + FieldSize(f, s));
        }
    }

    // Parent value at a child sample, blended in time between the two snapshots.
    [[nodiscard]] float ParentValue(const int field, const std::uint32_t i, const std::uint32_t j, const std::uint32_t k,
        const float blend) const
    {
        const FastStormConfig& p = cfg.parent;
        const Stagger st = kStagger[static_cast<std::size_t>(field)];
        const float r = static_cast<float>(cfg.ratio);
        const float fx = static_cast<float>(originI) + (static_cast<float>(i) + st.x) / r - st.x;
        const float fy = static_cast<float>(originJ) + (static_cast<float>(j) + st.y) / r - st.y;
        const float flx = std::floor(fx);
        const float fly = std::floor(fy);
        const float tx = fx - flx;
        const float ty = fy - fly;
        const auto i0 = static_cast<std::int32_t>(flx);
        const auto j0 = static_cast<std::int32_t>(fly);
        const auto mx = static_cast<std::int32_t>(p.nx) - 1;
        const auto my = static_cast<std::int32_t>(p.ny) - 1;
        const std::size_t plane = static_cast<std::size_t>(p.nx) * p.ny;
        const auto sample = [&](const std::vector<float>& data)
        {
            const std::size_t base = static_cast<std::size_t>(k) * plane;
            const auto at = [&](const std::int32_t ii, const std::int32_t jj)
            {
                return data[base + static_cast<std::size_t>(jj & my) * p.nx + static_cast<std::size_t>(ii & mx)];
            };
            const float a = at(i0, j0) + (at(i0 + 1, j0) - at(i0, j0)) * tx;
            const float b = at(i0, j0 + 1) + (at(i0 + 1, j0 + 1) - at(i0, j0 + 1)) * tx;
            return a + (b - a) * ty;
        };
        const float older = sample(before[static_cast<std::size_t>(field)]);
        const float newer = sample(after[static_cast<std::size_t>(field)]);
        return older + (newer - older) * blend;
    }

    void FillChildFromParent()
    {
        const FastStormState s = child->MutableState();
        const auto pointers = Pointers(s);
        for (int f = 0; f < kFields; ++f)
        {
            if (pointers[f] == nullptr || (f >= 7 && !ice))
            {
                continue;
            }
            const std::uint32_t levels = f == 2 ? s.nz + 1U : s.nz;
            for (std::uint32_t k = 0; k < levels; ++k)
            {
                for (std::uint32_t j = 0; j < s.ny; ++j)
                {
                    for (std::uint32_t i = 0; i < s.nx; ++i)
                    {
                        pointers[f][(static_cast<std::size_t>(k) * s.ny + j) * s.nx + i] = ParentValue(f, i, j, k, 1.0F);
                    }
                }
            }
        }
    }

    // Pre-projection hook of the child: pull the edge band toward the parent.
    void Relax(const FastStormState& s, const double endTime, const float dt)
    {
        ++childSteps;
        const float blend = static_cast<float>(std::clamp((endTime - t0) / std::max(t1 - t0, 1.0e-6), 0.0, 1.0));
        const float edge = 1.0F - std::exp(-dt / std::max(cfg.relaxSeconds, 1.0e-3F));
        const auto width = static_cast<std::int32_t>(std::min(cfg.relaxCells, std::min(s.nx, s.ny) / 2U));
        const auto pointers = Pointers(s);
        for (int f = 0; f < kFields; ++f)
        {
            if (pointers[f] == nullptr || (f >= 7 && !ice))
            {
                continue;
            }
            const std::uint32_t first = f == 2 ? 1U : 0U;
            const std::uint32_t last = s.nz - 1U;   // w: interior faces only (rigid lid and floor)
            for (std::uint32_t k = first; k <= last; ++k)
            {
                for (std::int32_t j = 0; j < static_cast<std::int32_t>(s.ny); ++j)
                {
                    const std::int32_t dj = std::min(j, static_cast<std::int32_t>(s.ny) - 1 - j);
                    const bool rowInside = dj >= width;
                    for (std::int32_t i = 0; i < static_cast<std::int32_t>(s.nx); ++i)
                    {
                        if (rowInside && i == width)
                        {
                            i = static_cast<std::int32_t>(s.nx) - width - 1;   // skip the free interior of this row
                            continue;
                        }
                        const std::int32_t di = std::min(i, static_cast<std::int32_t>(s.nx) - 1 - i);
                        const std::int32_t d = std::min(di, dj);
                        if (d >= width)
                        {
                            continue;
                        }
                        const float ramp = std::cos(0.5F * static_cast<float>(kPi) * static_cast<float>(d) / static_cast<float>(width));
                        // The outer two cells are held to the parent outright (see storm.hlsl RelaxBand): the periodic child draws in air from
                        // its opposite edge at the inflow side.
                        const float weight = d < 2 ? 1.0F : edge * ramp * ramp;
                        float& value = pointers[f][(static_cast<std::size_t>(k) * s.ny + static_cast<std::size_t>(j)) * s.nx
                            + static_cast<std::size_t>(i)];
                        value += weight * (ParentValue(f, static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(j), k, blend) - value);
                    }
                }
            }
        }
    }

    // Two-way: nudge the parent toward the child's block averages, away from the child's relaxation band.
    void Feedback(const float dt)
    {
        const FastStormState ps = parent->MutableState();
        const FastStormState cs = child->MutableState();
        const auto pp = Pointers(ps);
        const auto cp = Pointers(cs);
        const std::uint32_t r = cfg.ratio;
        const std::uint32_t margin = (cfg.relaxCells + r - 1U) / r + 1U;
        if (cells <= 2U * margin)
        {
            return;
        }
        const float beta = 1.0F - std::exp(-dt / std::max(cfg.feedbackSeconds, 1.0e-3F));
        const float inverse = 1.0F / static_cast<float>(r * r);
        for (int f = 0; f < kFields; ++f)
        {
            if (pp[f] == nullptr || cp[f] == nullptr || (f >= 7 && !ice))
            {
                continue;
            }
            const std::uint32_t first = f == 2 ? 1U : 0U;
            const std::uint32_t last = ps.nz - 1U;
            for (std::uint32_t k = first; k <= last; ++k)
            {
                for (std::uint32_t jj = margin; jj < cells - margin; ++jj)
                {
                    for (std::uint32_t ii = margin; ii < cells - margin; ++ii)
                    {
                        const std::uint32_t ci = ii * r;
                        const std::uint32_t cj = jj * r;
                        float sum = 0.0F;
                        for (std::uint32_t b = 0; b < r; ++b)
                        {
                            for (std::uint32_t a = 0; a < r; ++a)
                            {
                                std::uint32_t x = ci + a;
                                std::uint32_t y = cj + b;
                                if (f == 0) { x = ci; }   // u: the coincident face, averaged over the block's rows
                                if (f == 1) { y = cj; }
                                sum += cp[f][(static_cast<std::size_t>(k) * cs.ny + y) * cs.nx + x];
                            }
                        }
                        const float average = sum * inverse;
                        float& value = pp[f][(static_cast<std::size_t>(k) * ps.ny + (originJ + jj)) * ps.nx + (originI + ii)];
                        value += beta * (average - value);
                    }
                }
            }
        }
    }

    // Updraft-weighted centroid of the parent (in parent cells) at about 3 km, for placing a new child.
    [[nodiscard]] bool UpdraftCentroid(const float threshold, std::uint32_t& ci, std::uint32_t& cj) const
    {
        const FastStormConfig& p = cfg.parent;
        const FastStormState s = parent->MutableState();
        const std::uint32_t kw = std::min(p.nz - 1U, std::max(1U, static_cast<std::uint32_t>(3000.0F / p.dz + 0.5F)));
        const std::size_t plane = static_cast<std::size_t>(p.nx) * p.ny;
        double weight = 0.0, sx = 0.0, sy = 0.0;
        for (std::uint32_t j = 0; j < p.ny; ++j)
        {
            for (std::uint32_t i = 0; i < p.nx; ++i)
            {
                const float value = s.w[static_cast<std::size_t>(kw) * plane + static_cast<std::size_t>(j) * p.nx + i];
                if (value > threshold)
                {
                    weight += value;
                    sx += value * i;
                    sy += value * j;
                }
            }
        }
        if (weight <= 0.0)
        {
            return false;
        }
        ci = static_cast<std::uint32_t>(sx / weight + 0.5);
        cj = static_cast<std::uint32_t>(sy / weight + 0.5);
        return true;
    }

    void Advance(const double seconds)
    {
        const double target = parent->Diagnostics().time + seconds;
        while (parent->Diagnostics().time < target - 1.0e-6)
        {
            Capture(before);
            t0 = parent->Diagnostics().time;
            const float dt = parent->Step(std::min(cfg.parent.maxTimeStep, static_cast<float>(target - t0)));
            Capture(after);
            t1 = parent->Diagnostics().time;
            ++parentSteps;

            if (cfg.adaptive)
            {
                const float peak = parent->Diagnostics().maxUpdraft;
                if (!child && peak >= cfg.createUpdraft)
                {
                    std::uint32_t ci = 0, cj = 0;
                    if (UpdraftCentroid(0.5F * cfg.createUpdraft, ci, cj))
                    {
                        // The child starts from the parent's end-of-step state (snapshots are taken inside CreateChild).
                        CreateChild(cfg.cells, ci - std::min(ci, cfg.cells / 2U), cj - std::min(cj, cfg.cells / 2U));
                        t0 = t1;
                    }
                }
                else if (child)
                {
                    lowTimer = peak < cfg.releaseUpdraft ? lowTimer + dt : 0.0F;
                    if (lowTimer >= cfg.releaseHoldSeconds)
                    {
                        child.reset();
                        cells = 0U;
                    }
                }
            }

            if (child)
            {
                child->SetFrame(parent->Config().frameU, parent->Config().frameV);
                while (child->Diagnostics().time < t1 - 1.0e-4)
                {
                    (void)child->Step(static_cast<float>(t1 - child->Diagnostics().time));
                }
                if (cfg.feedback)
                {
                    Feedback(dt);
                }
            }
        }
    }
};

NestedStormSolver::NestedStormSolver(const NestConfig& config) : impl_(std::make_unique<Impl>(config)) {}
NestedStormSolver::~NestedStormSolver() = default;

std::string NestedStormSolver::Validate(const NestConfig& config)
{
    if (const std::string problem = FastStormSolver::Validate(config.parent); !problem.empty())
    {
        return "parent: " + problem;
    }
    if (config.ratio != 2U && config.ratio != 4U)
    {
        return "ratio must be 2 or 4";
    }
    const std::uint32_t childCells = config.cells * config.ratio;
    if (childCells < 8U || !IsPowerOfTwo(childCells))
    {
        return "cells * ratio must be a power of two of at least 8";
    }
    if (config.cells + 4U > std::min(config.parent.nx, config.parent.ny))
    {
        return "the child must fit inside the parent with a margin of two cells each side";
    }
    if (!config.adaptive)
    {
        const std::uint32_t oi = config.originI != 0xFFFFFFFFU ? config.originI : (config.parent.nx - config.cells) / 2U;
        const std::uint32_t oj = config.originJ != 0xFFFFFFFFU ? config.originJ : (config.parent.ny - config.cells) / 2U;
        if (oi < 2U || oj < 2U || oi + config.cells + 2U > config.parent.nx || oj + config.cells + 2U > config.parent.ny)
        {
            return "the child origin leaves less than two parent cells of margin";
        }
    }
    if (config.relaxCells < 2U || config.relaxCells * 2U >= childCells)
    {
        return "relaxCells must be at least 2 and less than half the child";
    }
    if (config.adaptive && !(config.releaseUpdraft < config.createUpdraft))
    {
        return "releaseUpdraft must be below createUpdraft (hysteresis)";
    }
    return {};
}

void NestedStormSolver::Advance(const double seconds) { impl_->Advance(seconds); }
FastStormSolver& NestedStormSolver::Parent() { return *impl_->parent; }
bool NestedStormSolver::HasChild() const { return impl_->child != nullptr; }

FastStormSolver& NestedStormSolver::Child()
{
    if (!impl_->child)
    {
        throw std::logic_error("NestedStormSolver::Child called without a child");
    }
    return *impl_->child;
}

std::uint32_t NestedStormSolver::ChildCells() const { return impl_->child ? impl_->cells : 0U; }
std::uint32_t NestedStormSolver::ChildCreations() const { return impl_->creations; }
std::uint32_t NestedStormSolver::BudgetCoarsenings() const { return impl_->coarsenings; }

std::uint64_t NestedStormSolver::TotalCells() const
{
    const FastStormConfig& p = impl_->cfg.parent;
    std::uint64_t total = static_cast<std::uint64_t>(p.nx) * p.ny * p.nz;
    if (impl_->child)
    {
        total += static_cast<std::uint64_t>(impl_->childNx) * impl_->childNy * p.nz;
    }
    return total;
}

const NestConfig& NestedStormSolver::Config() const { return impl_->cfg; }

double NestedStormSolver::SubStepRatio() const
{
    return impl_->parentSteps == 0U ? 0.0
        : static_cast<double>(impl_->childSteps) / static_cast<double>(impl_->parentSteps);
}

double NestedStormSolver::RelaxedFraction() const
{
    if (!impl_->child)
    {
        return 0.0;
    }
    const double w = std::min<double>(impl_->cfg.relaxCells, impl_->childNx / 2.0);
    const double free = (impl_->childNx - 2.0 * w) * (impl_->childNy - 2.0 * w);
    return 1.0 - free / (static_cast<double>(impl_->childNx) * impl_->childNy);
}
} // namespace orbit::weather_lab

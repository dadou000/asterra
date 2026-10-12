#include <orbit/weather_gpu/NestedGpuSolver.hpp>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace orbit::weather_gpu
{
struct NestedGpuSolver::Impl
{
    NestedGpuConfig cfg;
    std::unique_ptr<GpuStormSolver> parent;
    std::unique_ptr<GpuStormSolver> child;
    std::uint32_t originI{0};
    std::uint32_t originJ{0};
    double lastMs{0.0};

    Impl(rhi::Device& device, const shader::Compiler& compiler, const NestedGpuConfig& config) : cfg(config)
    {
        const weather_lab::FastStormConfig& p = cfg.parent;
        originI = cfg.originI != 0xFFFFFFFFU ? cfg.originI : (p.nx - cfg.cells) / 2U;
        originJ = cfg.originJ != 0xFFFFFFFFU ? cfg.originJ : (p.ny - cfg.cells) / 2U;

        weather_lab::FastStormConfig c = p;
        c.nx = cfg.cells * cfg.ratio;
        c.ny = cfg.cells * cfg.ratio;
        c.dx = p.dx / static_cast<float>(cfg.ratio);
        c.dy = p.dy / static_cast<float>(cfg.ratio);
        c.trackStorm = false;
        c.lateralSpongeCells = 0U;   // its edge is the relaxation band

        GpuStormOptions parentOptions;
        parentOptions.snapshot = true;
        parentOptions.cloudExtraCells = c.nx * c.ny * c.nz;   // the child's cells; the influence map follows them
        parent = std::make_unique<GpuStormSolver>(device, compiler, p, parentOptions);
        child = std::make_unique<GpuStormSolver>(device, compiler, c, GpuStormOptions{});
    }

    [[nodiscard]] ParentLink Link(const float blend) const
    {
        ParentLink link;
        link.parent = parent.get();
        link.originI = originI;
        link.originJ = originJ;
        link.ratio = cfg.ratio;
        link.relaxCells = cfg.relaxCells;
        link.relaxSeconds = cfg.relaxSeconds;
        link.blend = blend;
        return link;
    }
};

NestedGpuSolver::NestedGpuSolver(rhi::Device& device, const shader::Compiler& compiler, const NestedGpuConfig& config)
    : impl_(std::make_unique<Impl>(device, compiler, config))
{
}

NestedGpuSolver::~NestedGpuSolver() = default;

std::string NestedGpuSolver::Validate(const NestedGpuConfig& config)
{
    if (const std::string problem = GpuStormSolver::Validate(config.parent); !problem.empty())
    {
        return "parent: " + problem;
    }
    if (config.ratio != 2U && config.ratio != 4U)
    {
        return "ratio must be 2 or 4";
    }
    const std::uint32_t childCells = config.cells * config.ratio;
    if (childCells < 16U || childCells > 256U || (childCells & (childCells - 1U)) != 0U)
    {
        return "cells * ratio must be a power of two from 16 to 256";
    }
    if (config.cells + 4U > std::min(config.parent.nx, config.parent.ny))
    {
        return "the child must fit inside the parent with a margin of two cells each side";
    }
    const std::uint32_t oi = config.originI != 0xFFFFFFFFU ? config.originI : (config.parent.nx - config.cells) / 2U;
    const std::uint32_t oj = config.originJ != 0xFFFFFFFFU ? config.originJ : (config.parent.ny - config.cells) / 2U;
    if (oi < 2U || oj < 2U || oi + config.cells + 2U > config.parent.nx || oj + config.cells + 2U > config.parent.ny)
    {
        return "the child origin leaves less than two parent cells of margin";
    }
    if (config.relaxCells < 2U || config.relaxCells * 2U >= childCells)
    {
        return "relaxCells must be at least 2 and less than half the child";
    }
    if (config.childSubsteps < 1U || config.childSubsteps > 4U)
    {
        return "childSubsteps must be 1 to 4";
    }
    return {};
}

std::string NestedGpuSolver::ReloadShaders(const std::string_view source)
{
    Impl& s = *impl_;
    // Same source for both levels: compile the parent first and only swap the child when the parent compiled.
    if (std::string error = s.parent->ReloadShaders(source); !error.empty())
    {
        return error;
    }
    return s.child->ReloadShaders(source);
}

void NestedGpuSolver::Upload(weather_lab::FastStormSolver& cpuParent)
{
    Impl& s = *impl_;
    s.parent->Upload(cpuParent);
    s.child->FillFromParent(s.Link(1.0F));
}

void NestedGpuSolver::Advance(const std::uint32_t steps, const float dt)
{
    Impl& s = *impl_;
    double total = 0.0;
    const float childDt = dt / static_cast<float>(s.cfg.childSubsteps);
    for (std::uint32_t i = 0; i < steps; ++i)
    {
        s.parent->Advance(1U, dt);
        total += s.parent->LastAdvanceMilliseconds();
        for (std::uint32_t m = 1; m <= s.cfg.childSubsteps; ++m)
        {
            s.child->Advance(1U, childDt, s.Link(static_cast<float>(m) / static_cast<float>(s.cfg.childSubsteps)));
            total += s.child->LastAdvanceMilliseconds();
        }
    }
    s.lastMs = total;
}

void NestedGpuSolver::ExportCloud(StormPlacement placement)
{
    Impl& s = *impl_;
    const weather_lab::FastStormConfig& p = s.cfg.parent;
    const weather_lab::FastStormConfig& c = s.child->Config();
    placement.child.present = true;
    placement.child.dx = c.dx;
    placement.child.dy = c.dy;
    placement.child.nx = c.nx;
    placement.child.ny = c.ny;
    placement.child.nz = c.nz;
    // Child centre from the parent centre, in metres.
    placement.child.offsetX = (static_cast<float>(s.originI) + 0.5F * static_cast<float>(s.cfg.cells) - 0.5F * static_cast<float>(p.nx)) * p.dx;
    placement.child.offsetY = (static_cast<float>(s.originJ) + 0.5F * static_cast<float>(s.cfg.cells) - 0.5F * static_cast<float>(p.ny)) * p.dy;
    placement.child.dataOffset = 32U + s.parent->CellCount();
    placement.child.blendMeters = static_cast<float>(s.cfg.relaxCells) * c.dx;
    s.parent->ExportCloud(placement);
    s.child->ExportCloudInto(s.parent->CloudBuffer(), placement.child.dataOffset);
    s.child->ExportCloudInto(s.parent->CloudBuffer(), s.parent->RainOffset() + s.parent->CellCount(), true);
}

void NestedGpuSolver::ChildWestSouthEdge(float& east, float& north) const
{
    const Impl& s = *impl_;
    const weather_lab::FastStormConfig& p = s.cfg.parent;
    east = (static_cast<float>(s.originI) - 0.5F * static_cast<float>(p.nx)) * p.dx;
    north = (static_cast<float>(s.originJ) - 0.5F * static_cast<float>(p.ny)) * p.dy;
}

rhi::Buffer& NestedGpuSolver::CloudBuffer() { return impl_->parent->CloudBuffer(); }

StormCheckpoint NestedGpuSolver::Save()
{
    Impl& s = *impl_;
    StormCheckpoint out = s.parent->Save();
    const StormCheckpoint child = s.child->Save();
    out.data.insert(out.data.end(), child.data.begin(), child.data.end());
    return out;
}

void NestedGpuSolver::Restore(const StormCheckpoint& checkpoint)
{
    Impl& s = *impl_;
    const std::size_t parentFloats = s.parent->CheckpointBytes() / sizeof(float);
    const std::size_t childFloats = s.child->CheckpointBytes() / sizeof(float);
    if (checkpoint.data.size() != parentFloats + childFloats)
    {
        throw std::runtime_error("NestedGpuSolver::Restore: the checkpoint is from a different nest");
    }
    StormCheckpoint parentPart;
    parentPart.time = checkpoint.time;
    parentPart.data.assign(checkpoint.data.begin(), checkpoint.data.begin() + static_cast<std::ptrdiff_t>(parentFloats));
    StormCheckpoint childPart;
    childPart.time = checkpoint.time;
    childPart.data.assign(checkpoint.data.begin() + static_cast<std::ptrdiff_t>(parentFloats), checkpoint.data.end());
    s.parent->Restore(parentPart);
    s.child->Restore(childPart);
}

std::size_t NestedGpuSolver::CheckpointBytes() const
{
    return impl_->parent->CheckpointBytes() + impl_->child->CheckpointBytes();
}

double NestedGpuSolver::Time() const { return impl_->parent->Time(); }
double NestedGpuSolver::LastAdvanceMilliseconds() const { return impl_->lastMs; }
std::size_t NestedGpuSolver::ResidentBytes() const { return impl_->parent->ResidentBytes() + impl_->child->ResidentBytes(); }
GpuStormSolver& NestedGpuSolver::Parent() { return *impl_->parent; }
GpuStormSolver& NestedGpuSolver::Child() { return *impl_->child; }
const NestedGpuConfig& NestedGpuSolver::Config() const { return impl_->cfg; }
} // namespace orbit::weather_gpu

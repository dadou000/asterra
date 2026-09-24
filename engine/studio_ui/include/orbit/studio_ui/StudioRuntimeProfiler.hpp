#pragma once

#include <orbit/scene/ObjectStore.hpp>
#include <orbit/volume_fields/VolumeFieldStorage.hpp>
#include <orbit/volume_render/UniversalVolumeRenderer.hpp>
#include <orbit/volume_solver/SurfaceVolumeSolver.hpp>

#include <utility>

namespace orbit::studio_ui
{
struct StudioVolumeRuntimeProfilerSnapshot
{
    scene::ObjectId volume{};
    volume_fields::VolumeFieldDiagnostics fields{};
    volume_solver::SurfaceVolumeSolverDiagnostics solver{};
    volume_render::VolumeRenderDiagnostics renderer{};
    u32 invalidatedTiles{0U};

    bool hasSelection{false};
    bool hasFields{false};
    bool hasSolver{false};
    bool hasRenderer{false};
};

[[nodiscard]] inline StudioVolumeRuntimeProfilerSnapshot&
MutableStudioVolumeRuntimeProfiler() noexcept
{
    static StudioVolumeRuntimeProfilerSnapshot snapshot{};
    return snapshot;
}

[[nodiscard]] inline const StudioVolumeRuntimeProfilerSnapshot&
StudioVolumeRuntimeProfiler() noexcept
{
    return MutableStudioVolumeRuntimeProfiler();
}

inline void PublishStudioVolumeRuntimeProfiler(
    StudioVolumeRuntimeProfilerSnapshot snapshot) noexcept
{
    MutableStudioVolumeRuntimeProfiler() =
        std::move(snapshot);
}

inline void ResetStudioVolumeRuntimeProfiler() noexcept
{
    MutableStudioVolumeRuntimeProfiler() = {};
}
} // namespace orbit::studio_ui

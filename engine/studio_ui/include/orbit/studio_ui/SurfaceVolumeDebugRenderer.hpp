#pragma once

#include <orbit/render_view/RenderView.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/volume_fields/VolumeFieldStorage.hpp>
#include <orbit/volume_solver/SurfaceVolumeSolver.hpp>
#include <orbit/world_model/VolumeSchemas.hpp>

#include <memory>

namespace orbit::studio_ui
{
class SurfaceVolumeDebugRenderer
{
public:
    SurfaceVolumeDebugRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);

    void Draw(
        rhi::CommandList& commands,
        rhi::Texture& target,
        u32 width,
        u32 height,
        const render_view::CameraState& camera,
        const world_model::ResolvedVolumeDomain& domain,
        const volume_fields::VolumeFieldDiagnostics& fields,
        volume_solver::SurfaceVolumeDebugView view,
        volume_solver::VolumeSliceAxis sliceAxis,
        world_model::VolumeField fieldChannel,
        u32 debugLayer,
        rhi::Buffer& field,
        rhi::Buffer& residency);

private:
    std::unique_ptr<rhi::GraphicsPipeline>
        pipeline_;
};
} // namespace orbit::studio_ui

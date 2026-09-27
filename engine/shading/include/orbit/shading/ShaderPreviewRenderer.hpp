#pragma once

#include <orbit/content/RuntimeTexture.hpp>
#include <orbit/core/Types.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/rhi/Queue.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/shading/ObjMesh.hpp>
#include <orbit/shading/ShaderProgram.hpp>
#include <orbit/shading/ShadingContract.hpp>

#include <array>
#include <memory>
#include <span>
#include <string>

namespace orbit::shading
{
// Draws the Shading tab's preview: a background, then one shape shaded by the
// current user program.
//
// Hot swap contract (docs/ORBIT_HOT_ITERATION.md section 9): a new program
// becomes a new pipeline; the previous one is not destroyed while frames that
// may still reference it are in flight, it is retired after a fixed number of
// frames. A program that fails to build a pipeline leaves the working
// pipeline in place. With no valid program the preview draws a magenta error
// checker instead of going blank.
class ShaderPreviewRenderer
{
public:
    // Frames a replaced pipeline stays alive. Comfortably above the swapchain
    // depth so no in-flight command buffer can still use it.
    static constexpr u32 kRetireAfterFrames = 8U;

    // `graphicsQueue` is Studio's one shared graphics queue (see Main.cpp);
    // this renderer submits its own small one-shot command lists on it to
    // upload texture2d parameters, synchronously, off the hot per-frame path
    // (a shader/material select or a hot-reloaded image, not every frame).
    ShaderPreviewRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler,
        rhi::Queue& graphicsQueue);
    ~ShaderPreviewRenderer();

    ShaderPreviewRenderer(const ShaderPreviewRenderer&) = delete;
    ShaderPreviewRenderer& operator=(const ShaderPreviewRenderer&) = delete;

    struct UpdateResult
    {
        bool replaced{false};
        // Non-empty when `program` could not become a pipeline.
        std::string error;
        // Non-empty when the mesh could not be uploaded; the previous mesh
        // stays on screen.
        std::string meshError;
        // Non-empty when a texture2d parameter's image could not be
        // uploaded; the previous texture in that slot (if any) stays bound.
        std::array<std::string, kMaxShaderTextures> textureErrors;
    };

    // Call once per frame before recording. `program` is the workspace's last
    // good program (null for none) and `revision` its ProgramRevision().
    // `mesh` is the preview mesh (null for none) and `meshRevision` its
    // revision; a new revision uploads new buffers and retires the old ones the
    // same way as pipelines. `textures`/`textureRevisions` are the decoded
    // CPU pixels (from the workspace's texture2d parameters, indexed by
    // ShaderTextureDecl::slot) and a per-slot revision; a new revision
    // uploads that slot's GPU texture and retires the old one.
    UpdateResult Update(
        const ShadingProgram* program,
        u64 revision,
        const MeshData* mesh = nullptr,
        u64 meshRevision = 0U,
        std::array<const content::RuntimeTexture*, kMaxShaderTextures>
            textures = {},
        std::array<u64, kMaxShaderTextures> textureRevisions = {});

    // Records the preview into `color`/`depth`; both must already be bound as
    // render targets by the graph pass (color RGBA16F, depth D32 reverse-Z).
    void Draw(
        rhi::CommandList& commands,
        rhi::Texture& color,
        rhi::Texture& depth,
        u32 width,
        u32 height,
        const PreviewState& state,
        std::span<const f32, kMaxParameterFloats> parameters,
        f32 timeSeconds);

    [[nodiscard]] bool UsingErrorShader() const noexcept;
    // True when a mesh is uploaded and PreviewShape::Mesh will draw it.
    [[nodiscard]] bool HasMesh() const noexcept;
    [[nodiscard]] u32 RetiredPipelineCount() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::shading

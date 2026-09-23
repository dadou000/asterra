from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one seam, found {count}")
    return text.replace(old, new, 1)


root = Path(__file__).resolve().parents[1]
terrain_path = root / "engine/terrain_render/src/TerrainPreviewRenderer.cpp"
studio_path = root / "engine/studio_ui/src/StudioViewportRenderer.cpp"

terrain = terrain_path.read_text(encoding="utf-8")

terrain = replace_once(
    terrain,
    '#include <orbit/terrain_render/TerrainPreviewRenderer.hpp>\n#include "TerrainSurfaceShader.hpp"\n',
    '#include <orbit/terrain_render/TerrainPreviewRenderer.hpp>\n#include <orbit/terrain_render/SurfaceEffectGpuBinding.hpp>\n#include <orbit/terrain_render/SurfaceEffectShader.hpp>\n#include "TerrainSurfaceShader.hpp"\n',
    "terrain includes",
)

terrain = replace_once(
    terrain,
    '          config_(std::move(config)),\n          baseClipmapConfig_(\n',
    '          config_(std::move(config)),\n          surfaceEffects_(\n              device,\n              config_.framesInFlight),\n          baseClipmapConfig_(\n',
    "terrain effect binding initializer",
)

terrain = replace_once(
    terrain,
    '    void Draw(\n        rhi::CommandList& commandList,\n',
    '    void SetSurfaceEffects(\n        const std::span<const SurfaceEffectGpuStamp> effects)\n    {\n        surfaceEffects_.Set(effects);\n    }\n\n    void Draw(\n        rhi::CommandList& commandList,\n',
    "terrain effect setter",
)

terrain = replace_once(
    terrain,
    '        commandList.\n            SetGraphicsPipeline(\n                *pipeline_);\n\n        for (u32 levelIndex = 0;\n',
    '        commandList.\n            SetGraphicsPipeline(\n                *pipeline_);\n\n        surfaceEffects_.Bind(\n            commandList,\n            frameIndex);\n\n        for (u32 levelIndex = 0;\n',
    "terrain effect bind",
)

terrain = replace_once(
    terrain,
    '        const shader::Binary\n            pixelShader =\n                shaderCompiler.Compile({\n                    .source =\n                        kPixelShader,\n',
    '        const std::string pixelShaderSource =\n            BuildSurfaceEffectPixelShader(\n                kPixelShader);\n\n        const shader::Binary\n            pixelShader =\n                shaderCompiler.Compile({\n                    .source =\n                        pixelShaderSource,\n',
    "terrain effect shader source",
)

terrain = replace_once(
    terrain,
    '                    .pushConstantDwords = 56,\n                    .shaderResourceBuffers = 1,\n',
    '                    .pushConstantDwords = 56,\n                    .shaderResourceBuffers = 2,\n',
    "terrain pipeline SRV count",
)

terrain = replace_once(
    terrain,
    '    TerrainPreviewConfig config_;\n    terrain_view::ClipmapConfig\n        baseClipmapConfig_{};\n',
    '    TerrainPreviewConfig config_;\n    SurfaceEffectGpuBinding surfaceEffects_;\n    terrain_view::ClipmapConfig\n        baseClipmapConfig_{};\n',
    "terrain effect member",
)

terrain = replace_once(
    terrain,
    'void TerrainPreviewRenderer::Draw(\n    rhi::CommandList& commandList,\n',
    'void TerrainPreviewRenderer::SetSurfaceEffects(\n    const std::span<const SurfaceEffectGpuStamp> effects)\n{\n    impl_->SetSurfaceEffects(effects);\n}\n\nvoid TerrainPreviewRenderer::Draw(\n    rhi::CommandList& commandList,\n',
    "terrain public effect setter",
)

terrain_path.write_text(terrain, encoding="utf-8")

studio = studio_path.read_text(encoding="utf-8")
studio = replace_once(
    studio,
    '#include <orbit/studio_ui/StudioTerrainOverlayGeometry.hpp>\n',
    '#include <orbit/studio_ui/StudioTerrainOverlayGeometry.hpp>\n#include <orbit/studio_ui/VolumeSurfaceEffectRenderBridge.hpp>\n',
    "studio effect bridge include",
)

studio = replace_once(
    studio,
    '            terrain.renderer->\n                SetPhysicalPages(\n                    physicalPages.pages,\n                    physicalPages.generation);\n\n            const auto camera =\n',
    '            terrain.renderer->\n                SetPhysicalPages(\n                    physicalPages.pages,\n                    physicalPages.generation);\n\n            const auto surfaceEffects =\n                BuildVolumeSurfaceEffectRenderBatch(\n                    terrainRuntime->body,\n                    studio_session::VolumeSurfaceEffects().Stamps(),\n                    terrain_render::SurfaceEffectGpuBinding::MaximumStampCount);\n\n            terrain.renderer->\n                SetSurfaceEffects(\n                    surfaceEffects.stamps);\n\n            const auto camera =\n',
    "studio near-field effect feed",
)

studio_path.write_text(studio, encoding="utf-8")

print("M38 near-field terrain effects patched successfully")

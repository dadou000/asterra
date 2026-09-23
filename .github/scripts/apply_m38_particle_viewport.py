from pathlib import Path

HEADER = Path('engine/studio_ui/include/orbit/studio_ui/StudioViewportRenderer.hpp')
SOURCE = Path('engine/studio_ui/src/StudioViewportRenderer.cpp')


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f'{label}: expected exactly one seam, found {count}')
    return text.replace(old, new, 1)


header = HEADER.read_text(encoding='utf-8')
header = replace_once(
    header,
    '#include <orbit/volume_render/UniversalVolumeRenderer.hpp>\n',
    '#include <orbit/volume_render/UniversalVolumeRenderer.hpp>\n#include <orbit/volume_render/VolumeParticleRenderer.hpp>\n',
    'particle renderer include')
header = replace_once(
    header,
    '    volume_render::UniversalVolumeRenderer universalVolumeRenderer_;\n',
    '    volume_render::UniversalVolumeRenderer universalVolumeRenderer_;\n    volume_render::VolumeParticleRenderer volumeParticleRenderer_;\n',
    'particle renderer member')
HEADER.write_text(header, encoding='utf-8')

source = SOURCE.read_text(encoding='utf-8')
source = replace_once(
    source,
    '#include <orbit/studio_ui/VolumeSurfaceEffectRenderBridge.hpp>\n',
    '#include <orbit/studio_ui/VolumeSurfaceEffectRenderBridge.hpp>\n#include <orbit/studio_ui/VolumeParticleRenderBridge.hpp>\n',
    'particle render bridge include')
source = replace_once(
    source,
    '      universalVolumeRenderer_(device, compiler),\n      debugComposite_(device, compiler),\n',
    '      universalVolumeRenderer_(device, compiler),\n      volumeParticleRenderer_(device, compiler, framesInFlight),\n      debugComposite_(device, compiler),\n',
    'particle renderer construction')

marker = '''        {
            auto& histogram =
                luminanceHistogramPresentations_[
                    info.id];
'''
particle_block = '''        {
            // M38 particle output is an exactly-once simulation packet. Build
            // this viewport's GPU packet relative to the camera itself so the
            // float representation retains local precision at planetary scale.
            const auto particleSpawns =
                BuildVolumeParticleRenderBatch(
                    studio_session::VolumeParticleOutputs().Events(),
                    view->Camera().localPositionMeters);

            if (!particleSpawns.empty())
            {
                volumeParticleRenderer_.SetSpawns(
                    particleSpawns);

                const auto camera =
                    view->Camera();
                auto* particleColor =
                    color;
                auto* particleDepth =
                    &view->Depth();
                const u32 particleFrameIndex =
                    frameIndex % framesInFlight_;

                graph.AddPass(
                    prefix + ".VolumeParticles",
                    {
                        {
                            .texture = targets.color,
                            .state =
                                rhi::ResourceState::RenderTarget,
                            .access =
                                render_graph::Access::Write
                        },
                        {
                            .texture = targets.depth,
                            .state =
                                rhi::ResourceState::DepthRead,
                            .access =
                                render_graph::Access::Read
                        }
                    },
                    [this,
                     particleColor,
                     particleDepth,
                     width,
                     height,
                     camera,
                     particleFrameIndex](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        volumeParticleRenderer_.Draw(
                            commands,
                            *particleColor,
                            *particleDepth,
                            width,
                            height,
                            camera,
                            {},
                            particleFrameIndex);
                    });
            }
            else
            {
                volumeParticleRenderer_.SetSpawns({});
            }
        }

'''
source = replace_once(
    source,
    marker,
    particle_block + marker,
    'pre-histogram particle pass')
SOURCE.write_text(source, encoding='utf-8')

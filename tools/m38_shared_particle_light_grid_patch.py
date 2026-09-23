from pathlib import Path

def rep(path, old, new, count=1):
    p=Path(path); t=p.read_text(encoding='utf-8'); c=t.count(old)
    if c!=count: raise RuntimeError(f'{path}: expected {count} got {c}: {old[:120]!r}')
    p.write_text(t.replace(old,new,count),encoding='utf-8')

p='engine/volume_render/src/UniversalVolumeRendererBase.inc'

# Correct the shifted texture/sampler descriptor binding after inserting SRV slot 7.
rep(p,
'''[[vk::binding(8, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_depth;
[[vk::binding(7, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_depthSampler;''',
'''[[vk::binding(8, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_depth;
[[vk::binding(8, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_depthSampler;''')

# Shared particle lighting cache. Entry 0 is M38 presentation-space metadata;
# entry 1 is frame-space metadata for external consumers; cells begin at 2.
helper='''
float4 SampleParticleLightGrid(float3 framePosition)
{
    const uint4 meta = g_particleLightGrid[1];
    const float3 origin = float3(
        asfloat(meta.x),
        asfloat(meta.y),
        asfloat(meta.z));
    const float cellSize = asfloat(meta.w);

    if (!(cellSize > 0.0))
    {
        return 0.0;
    }

    const int3 cell =
        int3(floor((framePosition - origin) / cellSize));
    const int resolution = 32;

    if (any(cell < 0) ||
        any(cell >= int3(resolution,resolution,resolution)))
    {
        return 0.0;
    }

    const uint index =
        2u +
        (uint(cell.z) * resolution + uint(cell.y)) * resolution +
        uint(cell.x);
    const uint4 packed = g_particleLightGrid[index];

    return float4(
        float(packed.x) / 4096.0,
        float3(packed.yzw) / 1024.0);
}

float ParticleGridTransmittance(
    float3 start,
    float3 direction,
    float maximumDistance)
{
    if (maximumDistance <= 1.0e-4)
    {
        return 1.0;
    }

    const float3 ray = normalize(direction);
    const float stepLength =
        max(maximumDistance / 6.0, 8.0);
    float opticalDepth = 0.0;

    [unroll]
    for (uint step = 1u; step <= 6u; ++step)
    {
        const float distance =
            min(stepLength * float(step), maximumDistance);
        opticalDepth +=
            SampleParticleLightGrid(
                start + ray * distance).x *
            0.18;
    }

    return exp(-min(opticalDepth,20.0));
}

'''
rep(p,'float HenyeyGreenstein(\n',helper+'float HenyeyGreenstein(\n')

# Dense particle smoke attenuates stellar light reaching the volume.
rep(p,
'''            const float stellarShadow =
                ShadowTransmittance(
                    worldPosition +
                        stellarDirection *
                            0.01,
                    stellarDirection,
                    1.0e20,
                    p,
                    cellSize,
                    tileEdge);''',
'''            const float stellarShadow =
                ShadowTransmittance(
                    worldPosition +
                        stellarDirection *
                            0.01,
                    stellarDirection,
                    1.0e20,
                    p,
                    cellSize,
                    tileEdge) *
                ParticleGridTransmittance(
                    worldPosition,
                    stellarDirection,
                    192.0);''')

# Same authority attenuates point/spot lighting before it scatters in volumes.
rep(p,
'''                const float shadow =
                    ShadowTransmittance(
                        worldPosition +
                            sampleToLight *
                                0.01,
                        sampleToLight,
                        lightDistance,
                        p,
                        cellSize,
                        tileEdge);''',
'''                const float shadow =
                    ShadowTransmittance(
                        worldPosition +
                            sampleToLight *
                                0.01,
                        sampleToLight,
                        lightDistance,
                        p,
                        cellSize,
                        tileEdge) *
                    ParticleGridTransmittance(
                        worldPosition,
                        sampleToLight,
                        min(lightDistance,192.0));''')

# Particle emission becomes low-frequency incident radiance for the universal
# volume. The quantized grid already stores scene-linear emissive authority.
rep(p,
'''            scattering =
                sigmaS *
                incident;''',
'''            incident +=
                SampleParticleLightGrid(
                    worldPosition).yzw;

            scattering =
                sigmaS *
                incident;''')

print('finished universal-volume particle light-grid sampling')

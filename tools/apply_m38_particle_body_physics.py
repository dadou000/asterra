from pathlib import Path


def replace_once(path: str, old: str, new: str):
    p = Path(path)
    text = p.read_text(encoding='utf-8')
    count = text.count(old)
    if count != 1:
        raise SystemExit(f'{path}: expected one seam, found {count}')
    p.write_text(text.replace(old, new, 1), encoding='utf-8')

# Studio authoritative handoff: enrich particle events from composed world.
replace_once(
    'engine/studio_session/src/StudioSession.cpp',
    '''    VolumeParticleOutputs().Consume(\n        particles.Drain());\n''',
    '''    VolumeParticleOutputs().Consume(\n        world.Objects(),\n        world.Universe(),\n        world.Surfaces(),\n        particles.Drain());\n''')

# Persistent GPU state layout + physics.
p = Path('engine/volume_render/src/VolumeParticleGpuState.cpp')
text = p.read_text(encoding='utf-8')
text = text.replace('''    float3 emissionColor;\n    float reserved1;\n};\n\nstruct Particle\n''', '''    float3 emissionColor;\n    float reserved1;\n    float3 bodyCenterMeters;\n    float gravitationalParameterM3PerS2;\n    float3 surfaceRadiiMeters;\n    float gravitySofteningMeters;\n    float physicalSurfaceEnabled;\n    float reserved2;\n    float reserved3;\n    float reserved4;\n};\n\nstruct Particle\n''', 1)
text = text.replace('''    float3 emissionColor;\n    uint generation;\n};\n''', '''    float3 emissionColor;\n    uint generation;\n    float3 bodyCenterMeters;\n    float gravitationalParameterM3PerS2;\n    float3 surfaceRadiiMeters;\n    float gravitySofteningMeters;\n    float physicalSurfaceEnabled;\n    float reserved0;\n    float reserved1;\n    float reserved2;\n};\n''', 1)
old = '''                particle.positionMeters += g.originDelta.xyz;\n                const float dragScale = exp(-max(particle.linearDragPerSecond, 0.0) * dt);\n                particle.velocityMetersPerSecond *= dragScale;\n                particle.positionMeters +=\n                    particle.velocityMetersPerSecond * dt;\n                AppendParticle(particle);\n'''
new = '''                particle.positionMeters += g.originDelta.xyz;\n                particle.bodyCenterMeters += g.originDelta.xyz;\n\n                const uint gravityMode = particle.behaviorFlags & 0x3u;\n                if (gravityMode == 1u &&\n                    particle.gravitationalParameterM3PerS2 > 0.0 &&\n                    particle.gravityScale > 0.0)\n                {\n                    const float3 radial = particle.positionMeters - particle.bodyCenterMeters;\n                    const float softening = max(particle.gravitySofteningMeters, 0.0);\n                    const float radius2 = dot(radial, radial) + softening * softening;\n                    if (radius2 > 1.0e-6)\n                    {\n                        const float inverseRadius = rsqrt(radius2);\n                        const float inverseRadius3 = inverseRadius * inverseRadius * inverseRadius;\n                        particle.velocityMetersPerSecond +=\n                            -radial * particle.gravitationalParameterM3PerS2 *\n                            particle.gravityScale * inverseRadius3 * dt;\n                    }\n                }\n\n                const float dragScale = exp(-max(particle.linearDragPerSecond, 0.0) * dt);\n                particle.velocityMetersPerSecond *= dragScale;\n                particle.positionMeters += particle.velocityMetersPerSecond * dt;\n\n                const uint collisionMode = (particle.behaviorFlags >> 2u) & 0x3u;\n                bool keepParticle = true;\n                if (collisionMode != 0u && particle.physicalSurfaceEnabled > 0.5)\n                {\n                    const float3 radii = max(particle.surfaceRadiiMeters, 0.001);\n                    const float3 local = particle.positionMeters - particle.bodyCenterMeters;\n                    const float normalizedRadius2 = dot(local / radii, local / radii);\n                    if (normalizedRadius2 <= 1.0)\n                    {\n                        if (collisionMode == 1u)\n                        {\n                            keepParticle = false;\n                        }\n                        else\n                        {\n                            const float scale = rsqrt(max(normalizedRadius2, 1.0e-12));\n                            const float3 surfacePoint = local * scale;\n                            const float3 normal = normalize(surfacePoint / (radii * radii));\n                            particle.positionMeters =\n                                particle.bodyCenterMeters + surfacePoint +\n                                normal * max(particle.radiusMeters, 0.001);\n                            const float normalSpeed = dot(particle.velocityMetersPerSecond, normal);\n                            if (normalSpeed < 0.0)\n                            {\n                                if (collisionMode == 2u)\n                                {\n                                    particle.velocityMetersPerSecond -= normal * normalSpeed;\n                                }\n                                else\n                                {\n                                    particle.velocityMetersPerSecond -=\n                                        normal * normalSpeed * (1.0 + saturate(particle.restitution));\n                                }\n                            }\n                        }\n                    }\n                }\n\n                if (keepParticle)\n                {\n                    AppendParticle(particle);\n                }\n'''
if old not in text:
    raise SystemExit('VolumeParticleGpuState.cpp: integration seam missing')
text = text.replace(old, new, 1)
old = '''        particle.emissionColor = max(spawn.emissionColor, 0.0);\n        particle.generation = g.counts.y;\n        AppendParticle(particle);\n'''
new = '''        particle.emissionColor = max(spawn.emissionColor, 0.0);\n        particle.bodyCenterMeters = spawn.bodyCenterMeters;\n        particle.gravitationalParameterM3PerS2 = max(spawn.gravitationalParameterM3PerS2, 0.0);\n        particle.surfaceRadiiMeters = max(spawn.surfaceRadiiMeters, 0.0);\n        particle.gravitySofteningMeters = max(spawn.gravitySofteningMeters, 0.0);\n        particle.physicalSurfaceEnabled = spawn.physicalSurfaceEnabled;\n        particle.reserved0 = 0.0;\n        particle.reserved1 = 0.0;\n        particle.reserved2 = 0.0;\n        particle.generation = g.counts.y;\n        AppendParticle(particle);\n'''
if old not in text:
    raise SystemExit('VolumeParticleGpuState.cpp: spawn seam missing')
text = text.replace(old, new, 1)
p.write_text(text, encoding='utf-8')

# Renderer must match the 144-byte persistent state layout.
p = Path('engine/volume_render/src/VolumeParticleRenderer.cpp')
text = p.read_text(encoding='utf-8')
old = '''    float3 emissionColor;\n    uint generation;\n};\n'''
new = '''    float3 emissionColor;\n    uint generation;\n    float3 bodyCenterMeters;\n    float gravitationalParameterM3PerS2;\n    float3 surfaceRadiiMeters;\n    float gravitySofteningMeters;\n    float physicalSurfaceEnabled;\n    float reserved0;\n    float reserved1;\n    float reserved2;\n};\n'''
if old not in text:
    raise SystemExit('VolumeParticleRenderer.cpp: particle layout seam missing')
text = text.replace(old, new, 1)
p.write_text(text, encoding='utf-8')

print('M38 particle body physics patch applied')

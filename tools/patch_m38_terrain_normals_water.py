from pathlib import Path


def replace_once(path, old, new):
    p = Path(path)
    text = p.read_text(encoding='utf-8')
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f'{path}: expected one seam, found {count}')
    p.write_text(text.replace(old, new, 1), encoding='utf-8')

# API: terrain refinement needs the simulation dt so standing water can reuse
# the already-authored linear drag without inventing a new coefficient.
replace_once(
    'engine/volume_render/include/orbit/volume_render/VolumeParticleGpuState.hpp',
    '''    void ApplyTerrainCollision(\n        rhi::CommandList& commands,\n        std::span<const VolumeParticleTerrainCollisionPage> pages);''',
    '''    void ApplyTerrainCollision(\n        rhi::CommandList& commands,\n        std::span<const VolumeParticleTerrainCollisionPage> pages,\n        f64 deltaSeconds);''')

replace_once(
    'engine/volume_render/include/orbit/volume_render/VolumeParticleRenderer.hpp',
    '''    void ApplyTerrainCollision(\n        rhi::CommandList& commands,\n        std::span<const VolumeParticleTerrainCollisionPage> pages);''',
    '''    void ApplyTerrainCollision(\n        rhi::CommandList& commands,\n        std::span<const VolumeParticleTerrainCollisionPage> pages,\n        f64 deltaSeconds);''')

replace_once(
    'engine/volume_render/src/VolumeParticleRenderer.cpp',
    '''void VolumeParticleRenderer::ApplyTerrainCollision(\n    rhi::CommandList& commands,\n    const std::span<const VolumeParticleTerrainCollisionPage> pages)\n{\n    state_.ApplyTerrainCollision(commands, pages);\n}''',
    '''void VolumeParticleRenderer::ApplyTerrainCollision(\n    rhi::CommandList& commands,\n    const std::span<const VolumeParticleTerrainCollisionPage> pages,\n    const f64 deltaSeconds)\n{\n    state_.ApplyTerrainCollision(commands, pages, deltaSeconds);\n}''')

replace_once(
    'engine/studio_ui/src/StudioViewportRenderer.cpp',
    '''                            volumeParticleRenderer_.ApplyTerrainCollision(\n                                commands,\n                                particleTerrainCollisionPages);''',
    '''                            volumeParticleRenderer_.ApplyTerrainCollision(\n                                commands,\n                                particleTerrainCollisionPages,\n                                particleDeltaSeconds);''')

state_path = Path('engine/volume_render/src/VolumeParticleGpuState.cpp')
state = state_path.read_text(encoding='utf-8')

helper_marker = '''bool SameBody(uint4 a, uint4 b)\n{\n    return all(a == b);\n}\n'''
helpers = r'''float3 CubeToDirection(uint face, float2 uv)
{
    float3 direction;
    if (face == 0u) direction = float3(1.0, uv.y, -uv.x);
    else if (face == 1u) direction = float3(-1.0, uv.y, uv.x);
    else if (face == 2u) direction = float3(uv.x, 1.0, -uv.y);
    else if (face == 3u) direction = float3(uv.x, -1.0, uv.y);
    else if (face == 4u) direction = float3(uv.x, uv.y, 1.0);
    else direction = float3(-uv.x, uv.y, -1.0);
    return normalize(direction);
}

float ReferenceRadius(float3 direction, float3 radii)
{
    float inverseRadius = sqrt(dot(direction / radii, direction / radii));
    return inverseRadius > 0.0 ? 1.0 / inverseRadius : 0.0;
}

float3 PhysicalSurfacePoint(uint face, float2 uv, float3 radii)
{
    float3 direction = CubeToDirection(face, uv);
    float elevation = SamplePhysicalPage(uv).x;
    return direction * (ReferenceRadius(direction, radii) + elevation);
}

float3 TerrainSurfaceNormal(float2 uv, float3 radii, float3 outward)
{
    float2 minimumUv = PageBoundsMin();
    float2 maximumUv = PageBoundsMax();
    float2 extent = max(maximumUv - minimumUv, float2(0.0000001, 0.0000001));
    float2 texelUv = extent / float(max(g.meta.x - 1u, 1u));

    float2 leftUv = clamp(uv - float2(texelUv.x, 0.0), minimumUv, maximumUv);
    float2 rightUv = clamp(uv + float2(texelUv.x, 0.0), minimumUv, maximumUv);
    float2 downUv = clamp(uv - float2(0.0, texelUv.y), minimumUv, maximumUv);
    float2 upUv = clamp(uv + float2(0.0, texelUv.y), minimumUv, maximumUv);

    float3 tangentU =
        PhysicalSurfacePoint(g.tile.x, rightUv, radii) -
        PhysicalSurfacePoint(g.tile.x, leftUv, radii);
    float3 tangentV =
        PhysicalSurfacePoint(g.tile.x, upUv, radii) -
        PhysicalSurfacePoint(g.tile.x, downUv, radii);

    float3 normal = cross(tangentU, tangentV);
    float normalLength2 = dot(normal, normal);
    if (normalLength2 <= 1.0e-12)
    {
        return normalize(outward);
    }

    normal *= rsqrt(normalLength2);
    if (dot(normal, outward) < 0.0)
    {
        normal = -normal;
    }
    return normal;
}

bool SameBody(uint4 a, uint4 b)
{
    return all(a == b);
}
'''
if state.count(helper_marker) != 1:
    raise RuntimeError(f'state helper seam count = {state.count(helper_marker)}')
state = state.replace(helper_marker, helpers, 1)

old_main = r'''    if (particle.generation != generation ||
        collisionMode == 0u ||
        (particle.behaviorFlags & (1u << 4u)) == 0u ||
        !SameBody(particle.bodyIdentity, pageBody))
    {
        return;
    }

    float3 local = particle.positionMeters - particle.bodyCenterMeters;
    float radialDistance = length(local);
    if (radialDistance <= 0.000001) return;

    float3 direction = local / radialDistance;
    uint face;
    float2 uv;
    DirectionToCube(direction, face, uv);
    if (!BelongsToPage(face, uv)) return;

    float elevationMeters = SamplePhysicalPage(uv).x;
    float3 radii = max(particle.surfaceRadiiMeters, 0.001);
    float inverseReferenceRadius = sqrt(dot(direction / radii, direction / radii));
    if (inverseReferenceRadius <= 0.0) return;

    float referenceRadius = 1.0 / inverseReferenceRadius;
    float terrainRadius = referenceRadius + elevationMeters;
    float particleRadius = max(particle.radiusMeters, 0.001);

    if (radialDistance > terrainRadius + particleRadius)
    {
        return;
    }

    if (collisionMode == 1u)
    {
        particle.generation = 0u;
        g_particles[index] = particle;
        return;
    }

    float3 surfacePoint = direction * terrainRadius;
    float3 normal = normalize(surfacePoint / (radii * radii));
    particle.positionMeters =
        particle.bodyCenterMeters + surfacePoint + normal * particleRadius;

    float normalSpeed = dot(particle.velocityMetersPerSecond, normal);
    if (normalSpeed < 0.0)
    {
        if (collisionMode == 2u)
        {
            particle.velocityMetersPerSecond -= normal * normalSpeed;
        }
        else
        {
            particle.velocityMetersPerSecond -=
                normal * normalSpeed * (1.0 + saturate(particle.restitution));
        }
    }

    g_particles[index] = particle;'''

new_main = r'''    if (particle.generation != generation ||
        (particle.behaviorFlags & (1u << 4u)) == 0u ||
        !SameBody(particle.bodyIdentity, pageBody))
    {
        return;
    }

    float3 local = particle.positionMeters - particle.bodyCenterMeters;
    float radialDistance = length(local);
    if (radialDistance <= 0.000001) return;

    float3 direction = local / radialDistance;
    uint face;
    float2 uv;
    DirectionToCube(direction, face, uv);
    if (!BelongsToPage(face, uv)) return;

    float2 physical = SamplePhysicalPage(uv);
    float elevationMeters = physical.x;
    float standingWaterDepthMeters = max(physical.y, 0.0);
    float3 radii = max(particle.surfaceRadiiMeters, 0.001);
    float referenceRadius = ReferenceRadius(direction, radii);
    if (referenceRadius <= 0.0) return;

    float terrainRadius = referenceRadius + elevationMeters;
    float particleRadius = max(particle.radiusMeters, 0.001);
    float3 normal = TerrainSurfaceNormal(uv, radii, direction);

    // Standing water is an environment, not a solid surface. Reuse the
    // particle's authored linear drag as the water-response coefficient and
    // scale it only by actual submersion. No hidden buoyancy/density model is
    // invented here.
    bool waterAffected = false;
    if (standingWaterDepthMeters > 0.0)
    {
        float waterRadius = terrainRadius + standingWaterDepthMeters;
        float submersion = saturate(
            (waterRadius + particleRadius - radialDistance) /
            max(2.0 * particleRadius, 0.001));
        if (submersion > 0.0)
        {
            float dt = max(asfloat(g.body.w), 0.0);
            float waterDrag = exp(
                -max(particle.linearDragPerSecond, 0.0) * dt * submersion);
            particle.velocityMetersPerSecond *= waterDrag;
            waterAffected = true;
        }
    }

    // CollisionMode::None still allows water drag, but never treats terrain as
    // a solid. Other modes refine the reference-ellipsoid fallback to the
    // actual physical terrain surface and slope normal.
    if (collisionMode == 0u ||
        radialDistance > terrainRadius + particleRadius)
    {
        if (waterAffected)
        {
            g_particles[index] = particle;
        }
        return;
    }

    if (collisionMode == 1u)
    {
        particle.generation = 0u;
        g_particles[index] = particle;
        return;
    }

    float3 surfacePoint = direction * terrainRadius;
    particle.positionMeters =
        particle.bodyCenterMeters + surfacePoint + normal * particleRadius;

    float normalSpeed = dot(particle.velocityMetersPerSecond, normal);
    if (normalSpeed < 0.0)
    {
        if (collisionMode == 2u)
        {
            particle.velocityMetersPerSecond -= normal * normalSpeed;
        }
        else
        {
            particle.velocityMetersPerSecond -=
                normal * normalSpeed * (1.0 + saturate(particle.restitution));
        }
    }

    g_particles[index] = particle;'''

if state.count(old_main) != 1:
    raise RuntimeError(f'state terrain-main seam count = {state.count(old_main)}')
state = state.replace(old_main, new_main, 1)

old_sig = '''void VolumeParticleGpuState::ApplyTerrainCollision(\n    rhi::CommandList& commands,\n    const std::span<const VolumeParticleTerrainCollisionPage> pages)'''
new_sig = '''void VolumeParticleGpuState::ApplyTerrainCollision(\n    rhi::CommandList& commands,\n    const std::span<const VolumeParticleTerrainCollisionPage> pages,\n    const f64 deltaSeconds)'''
if state.count(old_sig) != 1:
    raise RuntimeError(f'state signature seam count = {state.count(old_sig)}')
state = state.replace(old_sig, new_sig, 1)

old_constants = '''        constants[8] = page.bodyIdentity[2];\n        constants[9] = page.bodyIdentity[3];\n        constants[10] = MaximumParticleCount;'''
new_constants = '''        constants[8] = page.bodyIdentity[2];\n        constants[9] = page.bodyIdentity[3];\n        constants[10] = MaximumParticleCount;\n        const f32 terrainDeltaSeconds =\n            std::isfinite(deltaSeconds)\n                ? static_cast<f32>(std::clamp(deltaSeconds, 0.0, 1.0))\n                : 0.0F;\n        constants[11] = Bits(terrainDeltaSeconds);'''
if state.count(old_constants) != 1:
    raise RuntimeError(f'state constants seam count = {state.count(old_constants)}')
state = state.replace(old_constants, new_constants, 1)

state_path.write_text(state, encoding='utf-8')

print('M38 terrain normals + standing-water interaction patch applied')

from pathlib import Path


def replace_once(path, old, new):
    p = Path(path)
    text = p.read_text(encoding='utf-8')
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f'{path}: expected 1 match, got {count}: {old[:100]!r}')
    p.write_text(text.replace(old, new, 1), encoding='utf-8')

# World schema identifiers + resolved domain fields.
path='engine/world_model/include/orbit/world_model/VolumeSchemas.hpp'
replace_once(path,
'''inline constexpr schema::PropertyId kVolumeParticleRestitution{\n    .high = 0x4f52424954564f4cULL, .low = 0x5052545245535401ULL};\n''',
'''inline constexpr schema::PropertyId kVolumeParticleRestitution{\n    .high = 0x4f52424954564f4cULL, .low = 0x5052545245535401ULL};\ninline constexpr schema::PropertyId kVolumeParticleWaterDensityRatio{\n    .high = 0x4f52424954564f4cULL, .low = 0x5052545744525401ULL};\ninline constexpr schema::PropertyId kVolumeParticleWaterDrag{\n    .high = 0x4f52424954564f4cULL, .low = 0x5052545744524701ULL};\ninline constexpr schema::PropertyId kVolumeParticleWaterBuoyancyScale{\n    .high = 0x4f52424954564f4cULL, .low = 0x5052545742554f01ULL};\ninline constexpr schema::PropertyId kVolumeParticleKillOnWaterImmersion{\n    .high = 0x4f52424954564f4cULL, .low = 0x505254574b494c01ULL};\ninline constexpr schema::PropertyId kVolumeParticleSplashOnWaterEntry{\n    .high = 0x4f52424954564f4cULL, .low = 0x5052545753504c01ULL};\ninline constexpr schema::PropertyId kVolumeParticleWaterSplashScale{\n    .high = 0x4f52424954564f4cULL, .low = 0x5052545753505301ULL};\n''')
replace_once(path,
'''    f32 particleRestitution{0.25F};\n\n    u32 sourceCount{0U};\n''',
'''    f32 particleRestitution{0.25F};\n    // Relative to standing water: 1 = neutrally buoyant, <1 rises, >1 sinks.\n    f32 particleWaterDensityRatio{1.0F};\n    f32 particleWaterDragPerSecond{0.0F};\n    f32 particleWaterBuoyancyScale{1.0F};\n    bool particleKillOnWaterImmersion{false};\n    bool particleSplashOnWaterEntry{false};\n    f32 particleWaterSplashScale{1.0F};\n\n    u32 sourceCount{0U};\n''')

# Schema registration + resolver.
path='engine/world_model/src/VolumeSchemas.cpp'
replace_once(path,
'''            {.id=kVolumeParticleCollisionMode,.name="Particle Collision Mode",.kind=schema::PropertyKind::Integer,.defaultValue=i64{0},.range={.minimum=0.0,.maximum=3.0}},\n            {.id=kVolumeParticleRestitution,.name="Particle Restitution",.kind=schema::PropertyKind::Float,.defaultValue=0.25,.range={.minimum=0.0,.maximum=1.0}}\n''',
'''            {.id=kVolumeParticleCollisionMode,.name="Particle Collision Mode",.kind=schema::PropertyKind::Integer,.defaultValue=i64{0},.range={.minimum=0.0,.maximum=3.0}},\n            {.id=kVolumeParticleRestitution,.name="Particle Restitution",.kind=schema::PropertyKind::Float,.defaultValue=0.25,.range={.minimum=0.0,.maximum=1.0}},\n            {.id=kVolumeParticleWaterDensityRatio,.name="Particle Water Density Ratio",.kind=schema::PropertyKind::Float,.defaultValue=1.0,.range={.minimum=0.01,.maximum=100.0}},\n            {.id=kVolumeParticleWaterDrag,.name="Particle Water Drag",.kind=schema::PropertyKind::Float,.unit="1/s",.defaultValue=0.0,.range={.minimum=0.0,.maximum=1000.0}},\n            {.id=kVolumeParticleWaterBuoyancyScale,.name="Particle Water Buoyancy Scale",.kind=schema::PropertyKind::Float,.defaultValue=1.0,.range={.minimum=0.0,.maximum=16.0}},\n            {.id=kVolumeParticleKillOnWaterImmersion,.name="Particle Kill On Water Immersion",.kind=schema::PropertyKind::Boolean,.defaultValue=false},\n            {.id=kVolumeParticleSplashOnWaterEntry,.name="Particle Splash On Water Entry",.kind=schema::PropertyKind::Boolean,.defaultValue=false},\n            {.id=kVolumeParticleWaterSplashScale,.name="Particle Water Splash Scale",.kind=schema::PropertyKind::Float,.defaultValue=1.0,.range={.minimum=0.0,.maximum=64.0}}\n''')
replace_once(path,
'''        .particleCollisionMode = static_cast<VolumeParticleCollisionMode>(std::clamp<i64>(Read<i64>(objects, volume, kVolumeParticleCollisionMode, 0), 0, 3)),\n        .particleRestitution = static_cast<f32>(std::clamp(Read<f64>(objects, volume, kVolumeParticleRestitution, 0.25), 0.0, 1.0))\n''',
'''        .particleCollisionMode = static_cast<VolumeParticleCollisionMode>(std::clamp<i64>(Read<i64>(objects, volume, kVolumeParticleCollisionMode, 0), 0, 3)),\n        .particleRestitution = static_cast<f32>(std::clamp(Read<f64>(objects, volume, kVolumeParticleRestitution, 0.25), 0.0, 1.0)),\n        .particleWaterDensityRatio = static_cast<f32>(std::clamp(Read<f64>(objects, volume, kVolumeParticleWaterDensityRatio, 1.0), 0.01, 100.0)),\n        .particleWaterDragPerSecond = static_cast<f32>(std::clamp(Read<f64>(objects, volume, kVolumeParticleWaterDrag, 0.0), 0.0, 1000.0)),\n        .particleWaterBuoyancyScale = static_cast<f32>(std::clamp(Read<f64>(objects, volume, kVolumeParticleWaterBuoyancyScale, 1.0), 0.0, 16.0)),\n        .particleKillOnWaterImmersion = Read<bool>(objects, volume, kVolumeParticleKillOnWaterImmersion, false),\n        .particleSplashOnWaterEntry = Read<bool>(objects, volume, kVolumeParticleSplashOnWaterEntry, false),\n        .particleWaterSplashScale = static_cast<f32>(std::clamp(Read<f64>(objects, volume, kVolumeParticleWaterSplashScale, 1.0), 0.0, 64.0))\n''')

# M38 transport contract.
path='engine/volume_representation/include/orbit/volume_representation/VolumeOutputCoupling.hpp'
replace_once(path,
'''    world_model::VolumeParticleCollisionMode particleCollisionMode{world_model::VolumeParticleCollisionMode::None};\n    f32 particleRestitution{0.25F};\n\n    f32 surfaceDepositRatePerSecond{24.0F};\n''',
'''    world_model::VolumeParticleCollisionMode particleCollisionMode{world_model::VolumeParticleCollisionMode::None};\n    f32 particleRestitution{0.25F};\n    f32 particleWaterDensityRatio{1.0F};\n    f32 particleWaterDragPerSecond{0.0F};\n    f32 particleWaterBuoyancyScale{1.0F};\n    bool particleKillOnWaterImmersion{false};\n    bool particleSplashOnWaterEntry{false};\n    f32 particleWaterSplashScale{1.0F};\n\n    f32 surfaceDepositRatePerSecond{24.0F};\n''')
replace_once(path,
'''    world_model::VolumeParticleCollisionMode collisionMode{world_model::VolumeParticleCollisionMode::None};\n    f32 restitution{0.25F};\n};\n''',
'''    world_model::VolumeParticleCollisionMode collisionMode{world_model::VolumeParticleCollisionMode::None};\n    f32 restitution{0.25F};\n    f32 waterDensityRatio{1.0F};\n    f32 waterDragPerSecond{0.0F};\n    f32 waterBuoyancyScale{1.0F};\n    bool killOnWaterImmersion{false};\n    bool splashOnWaterEntry{false};\n    f32 waterSplashScale{1.0F};\n};\n''')

path='engine/volume_representation/src/VolumeOutputRuntime.cpp'
replace_once(path,
'''        settings.particleCollisionMode = domain->particleCollisionMode;\n        settings.particleRestitution = domain->particleRestitution;\n        settings.surfaceDepositRatePerSecond = domain->outputSurfaceDepositRatePerSecond;\n''',
'''        settings.particleCollisionMode = domain->particleCollisionMode;\n        settings.particleRestitution = domain->particleRestitution;\n        settings.particleWaterDensityRatio = domain->particleWaterDensityRatio;\n        settings.particleWaterDragPerSecond = domain->particleWaterDragPerSecond;\n        settings.particleWaterBuoyancyScale = domain->particleWaterBuoyancyScale;\n        settings.particleKillOnWaterImmersion = domain->particleKillOnWaterImmersion;\n        settings.particleSplashOnWaterEntry = domain->particleSplashOnWaterEntry;\n        settings.particleWaterSplashScale = domain->particleWaterSplashScale;\n        settings.surfaceDepositRatePerSecond = domain->outputSurfaceDepositRatePerSecond;\n''')

path='engine/volume_representation/src/VolumeOutputCoupling.cpp'
replace_once(path,
'''    const f32 particleRestitution = std::isfinite(entry.settings.particleRestitution) ? std::clamp(entry.settings.particleRestitution, 0.0F, 1.0F) : 0.25F;\n\n    batch.particles.reserve(particleBudget.admitted);\n''',
'''    const f32 particleRestitution = std::isfinite(entry.settings.particleRestitution) ? std::clamp(entry.settings.particleRestitution, 0.0F, 1.0F) : 0.25F;\n    const f32 particleWaterDensityRatio = std::isfinite(entry.settings.particleWaterDensityRatio) ? std::clamp(entry.settings.particleWaterDensityRatio, 0.01F, 100.0F) : 1.0F;\n    const f32 particleWaterDrag = std::isfinite(entry.settings.particleWaterDragPerSecond) ? std::clamp(entry.settings.particleWaterDragPerSecond, 0.0F, 1000.0F) : 0.0F;\n    const f32 particleWaterBuoyancy = std::isfinite(entry.settings.particleWaterBuoyancyScale) ? std::clamp(entry.settings.particleWaterBuoyancyScale, 0.0F, 16.0F) : 1.0F;\n    const f32 particleWaterSplashScale = std::isfinite(entry.settings.particleWaterSplashScale) ? std::clamp(entry.settings.particleWaterSplashScale, 0.0F, 64.0F) : 1.0F;\n\n    batch.particles.reserve(particleBudget.admitted);\n''')
replace_once(path,
'''            .collisionMode = entry.settings.particleCollisionMode,\n            .restitution = particleRestitution\n        });\n''',
'''            .collisionMode = entry.settings.particleCollisionMode,\n            .restitution = particleRestitution,\n            .waterDensityRatio = particleWaterDensityRatio,\n            .waterDragPerSecond = particleWaterDrag,\n            .waterBuoyancyScale = particleWaterBuoyancy,\n            .killOnWaterImmersion = entry.settings.particleKillOnWaterImmersion,\n            .splashOnWaterEntry = entry.settings.particleSplashOnWaterEntry,\n            .waterSplashScale = particleWaterSplashScale\n        });\n''')

# GPU records gain exactly one 16-byte water block.
path='engine/volume_render/include/orbit/volume_render/VolumeParticleGpuBinding.hpp'
replace_once(path,
'''    f32 gravitySofteningMeters{0.0F};\n\n    // Full 128-bit owning body identity. Surface-authority is encoded in\n''',
'''    f32 gravitySofteningMeters{0.0F};\n    f32 waterDensityRatio{1.0F};\n    f32 waterDragPerSecond{0.0F};\n    f32 waterBuoyancyScale{1.0F};\n    f32 waterSplashScale{1.0F};\n\n    // Full 128-bit owning body identity. Surface-authority is encoded in\n''')
replace_once(path,'static_assert(sizeof(VolumeParticleGpuSpawn) == 144U);','static_assert(sizeof(VolumeParticleGpuSpawn) == 160U);')
replace_once(path,'// increasing the 144-byte GPU record.','// adding only the authored 16-byte water-response block.')

path='engine/volume_render/include/orbit/volume_render/VolumeParticleGpuState.hpp'
replace_once(path,
'''    f32 gravitySofteningMeters{0.0F};\n    std::array<u32, 4U> bodyIdentity{};\n};\n\nstatic_assert(sizeof(VolumeParticleGpuStateRecord) == 144U);\n''',
'''    f32 gravitySofteningMeters{0.0F};\n    f32 waterDensityRatio{1.0F};\n    f32 waterDragPerSecond{0.0F};\n    f32 waterBuoyancyScale{1.0F};\n    f32 waterSplashScale{1.0F};\n    std::array<u32, 4U> bodyIdentity{};\n};\n\nstatic_assert(sizeof(VolumeParticleGpuStateRecord) == 160U);\n''')

# CPU -> GPU bridge and behavior flags. Bits 5/6 are authored; 30/31 are GPU runtime water state.
path='engine/studio_ui/include/orbit/studio_ui/VolumeParticleRenderBridge.hpp'
replace_once(path,
'''        const u32 behaviorFlags =\n            (static_cast<u32>(request.gravityMode) & 0x3U) |\n            ((static_cast<u32>(request.collisionMode) & 0x3U) << 2U);\n''',
'''        const u32 behaviorFlags =\n            (static_cast<u32>(request.gravityMode) & 0x3U) |\n            ((static_cast<u32>(request.collisionMode) & 0x3U) << 2U) |\n            (event.physics.hasPhysicalSurface ? (1U << 4U) : 0U) |\n            (request.killOnWaterImmersion ? (1U << 5U) : 0U) |\n            (request.splashOnWaterEntry ? (1U << 6U) : 0U);\n''')
# Remove old later OR of physical surface if present.
text=Path(path).read_text(encoding='utf-8')
text=text.replace('            .behaviorFlags = behaviorFlags |\n                (event.physics.hasPhysicalSurface ? (1U << 4U) : 0U),','            .behaviorFlags = behaviorFlags,')
Path(path).write_text(text,encoding='utf-8')
replace_once(path,
'''            .gravitySofteningMeters =\n                std::isfinite(event.physics.gravitySofteningMeters)\n                    ? static_cast<f32>(std::max(event.physics.gravitySofteningMeters, 0.0))\n                    : 0.0F,\n            .bodyIdentity = VolumeParticleBodyIdentityWords(event.owningBody)\n''',
'''            .gravitySofteningMeters =\n                std::isfinite(event.physics.gravitySofteningMeters)\n                    ? static_cast<f32>(std::max(event.physics.gravitySofteningMeters, 0.0))\n                    : 0.0F,\n            .waterDensityRatio = std::isfinite(request.waterDensityRatio) ? std::clamp(request.waterDensityRatio, 0.01F, 100.0F) : 1.0F,\n            .waterDragPerSecond = std::isfinite(request.waterDragPerSecond) ? std::clamp(request.waterDragPerSecond, 0.0F, 1000.0F) : 0.0F,\n            .waterBuoyancyScale = std::isfinite(request.waterBuoyancyScale) ? std::clamp(request.waterBuoyancyScale, 0.0F, 16.0F) : 1.0F,\n            .waterSplashScale = std::isfinite(request.waterSplashScale) ? std::clamp(request.waterSplashScale, 0.0F, 64.0F) : 1.0F,\n            .bodyIdentity = VolumeParticleBodyIdentityWords(event.owningBody)\n''')

# GPU simulation/HLSL layouts and water response.
path='engine/volume_render/src/VolumeParticleGpuState.cpp'
text=Path(path).read_text(encoding='utf-8')
# Both Spawn and Particle structs in simulation, plus Particle in terrain shader.
needle='''    float3 surfaceRadiiMeters;\n    float gravitySofteningMeters;\n    uint4 bodyIdentity;\n'''
if text.count(needle) != 3:
    raise RuntimeError(f'{path}: expected 3 GPU struct water insertion seams, got {text.count(needle)}')
text=text.replace(needle,
'''    float3 surfaceRadiiMeters;\n    float gravitySofteningMeters;\n    float waterDensityRatio;\n    float waterDragPerSecond;\n    float waterBuoyancyScale;\n    float waterSplashScale;\n    uint4 bodyIdentity;\n''')
old='''        particle.surfaceRadiiMeters = max(spawn.surfaceRadiiMeters, 0.0);\n        particle.gravitySofteningMeters = max(spawn.gravitySofteningMeters, 0.0);\n        particle.bodyIdentity = spawn.bodyIdentity;\n'''
new='''        particle.surfaceRadiiMeters = max(spawn.surfaceRadiiMeters, 0.0);\n        particle.gravitySofteningMeters = max(spawn.gravitySofteningMeters, 0.0);\n        particle.waterDensityRatio = max(spawn.waterDensityRatio, 0.01);\n        particle.waterDragPerSecond = max(spawn.waterDragPerSecond, 0.0);\n        particle.waterBuoyancyScale = max(spawn.waterBuoyancyScale, 0.0);\n        particle.waterSplashScale = max(spawn.waterSplashScale, 0.0);\n        particle.bodyIdentity = spawn.bodyIdentity;\n'''
if text.count(old)!=1: raise RuntimeError('spawn water copy seam mismatch')
text=text.replace(old,new,1)
old='''    // Standing water is an environment, not a solid surface. Reuse the\n    // particle's authored linear drag as the water-response coefficient and\n    // scale it only by actual submersion. No hidden buoyancy/density model is\n    // invented here.\n    bool waterAffected = false;\n    if (standingWaterDepthMeters > 0.0)\n    {\n        float waterRadius = terrainRadius + standingWaterDepthMeters;\n        float submersion = saturate(\n            (waterRadius + particleRadius - radialDistance) /\n            max(2.0 * particleRadius, 0.001));\n        if (submersion > 0.0)\n        {\n            float dt = max(asfloat(g.body.w), 0.0);\n            float waterDrag = exp(\n                -max(particle.linearDragPerSecond, 0.0) * dt * submersion);\n            particle.velocityMetersPerSecond *= waterDrag;\n            waterAffected = true;\n        }\n    }\n'''
new='''    // Standing-water response is authored per particle. Bit 31 tracks current\n    // immersion and bit 30 latches a water-entry event for the GPU splash\n    // output pass. No particle-state readback is required.\n    bool waterAffected = false;\n    bool killForWater = false;\n    const uint kWaterEntryPending = 1u << 30u;\n    const uint kWaterSubmerged = 1u << 31u;\n    bool wasSubmerged = (particle.behaviorFlags & kWaterSubmerged) != 0u;\n    bool nowSubmerged = false;\n    if (standingWaterDepthMeters > 0.0)\n    {\n        float waterRadius = terrainRadius + standingWaterDepthMeters;\n        float submersion = saturate(\n            (waterRadius + particleRadius - radialDistance) /\n            max(2.0 * particleRadius, 0.001));\n        nowSubmerged = submersion > 0.0;\n        if (nowSubmerged)\n        {\n            float dt = max(asfloat(g.body.w), 0.0);\n            float waterDrag = exp(\n                -max(particle.waterDragPerSecond, 0.0) * dt * submersion);\n            particle.velocityMetersPerSecond *= waterDrag;\n\n            // Main integration already applied gravity. Add only buoyancy here:\n            // a density ratio of 1 with scale 1 cancels body gravity at full\n            // submersion; ratios below/above 1 rise/sink respectively.\n            float3 radial = particle.positionMeters - particle.bodyCenterMeters;\n            float radius2 = dot(radial, radial);\n            if (radius2 > 1.0e-6 &&\n                particle.gravitationalParameterM3PerS2 > 0.0 &&\n                particle.gravityScale > 0.0 &&\n                particle.waterBuoyancyScale > 0.0)\n            {\n                float inverseRadius = rsqrt(radius2);\n                float gravityAcceleration =\n                    particle.gravitationalParameterM3PerS2 / radius2 *\n                    particle.gravityScale;\n                float buoyancyAcceleration = gravityAcceleration *\n                    particle.waterBuoyancyScale /\n                    max(particle.waterDensityRatio, 0.01);\n                particle.velocityMetersPerSecond +=\n                    radial * inverseRadius * buoyancyAcceleration *\n                    submersion * dt;\n            }\n\n            if (!wasSubmerged &&\n                (particle.behaviorFlags & (1u << 6u)) != 0u)\n            {\n                particle.behaviorFlags |= kWaterEntryPending;\n            }\n\n            // Kill only after the particle centre crosses the water surface,\n            // avoiding death from a grazing radius contact.\n            if ((particle.behaviorFlags & (1u << 5u)) != 0u &&\n                radialDistance <= waterRadius)\n            {\n                killForWater = true;\n            }\n            waterAffected = true;\n        }\n    }\n\n    if (nowSubmerged) particle.behaviorFlags |= kWaterSubmerged;\n    else particle.behaviorFlags &= ~kWaterSubmerged;\n\n    if (killForWater)\n    {\n        particle.generation = 0u;\n        g_particles[index] = particle;\n        return;\n    }\n'''
if text.count(old)!=1: raise RuntimeError('water response block mismatch')
text=text.replace(old,new,1)
Path(path).write_text(text,encoding='utf-8')

# Renderer graphics layout must remain byte-identical with persistent state.
path='engine/volume_render/src/VolumeParticleRenderer.cpp'
text=Path(path).read_text(encoding='utf-8')
needle='''    float3 surfaceRadiiMeters;\n    float gravitySofteningMeters;\n    uint4 bodyIdentity;\n'''
if text.count(needle)!=1: raise RuntimeError(f'{path}: renderer struct seam mismatch {text.count(needle)}')
text=text.replace(needle,
'''    float3 surfaceRadiiMeters;\n    float gravitySofteningMeters;\n    float waterDensityRatio;\n    float waterDragPerSecond;\n    float waterBuoyancyScale;\n    float waterSplashScale;\n    uint4 bodyIdentity;\n''',1)
Path(path).write_text(text,encoding='utf-8')

# Studio authored controls.
path='engine/studio_ui/src/VolumeAuthoringM36.cpp'
replace_once(path,
'''    f64 restitution = volume->particleRestitution;\n    if (context.InputDouble("Restitution##volume-output-restitution", restitution))\n        world.Commands().SetProperty(*volumeId, world_model::kVolumeParticleRestitution, std::clamp(restitution, 0.0, 1.0));\n\n    f64 depositRate = volume->outputSurfaceDepositRatePerSecond;\n''',
'''    f64 restitution = volume->particleRestitution;\n    if (context.InputDouble("Restitution##volume-output-restitution", restitution))\n        world.Commands().SetProperty(*volumeId, world_model::kVolumeParticleRestitution, std::clamp(restitution, 0.0, 1.0));\n\n    context.Text("Standing Water Response");\n    f64 waterDensityRatio = volume->particleWaterDensityRatio;\n    f64 waterDrag = volume->particleWaterDragPerSecond;\n    f64 waterBuoyancy = volume->particleWaterBuoyancyScale;\n    f64 waterSplashScale = volume->particleWaterSplashScale;\n    bool killOnWater = volume->particleKillOnWaterImmersion;\n    bool splashOnWaterEntry = volume->particleSplashOnWaterEntry;\n    if (context.InputDouble("Density Ratio to Water##volume-output-water-density", waterDensityRatio))\n        world.Commands().SetProperty(*volumeId, world_model::kVolumeParticleWaterDensityRatio, std::clamp(waterDensityRatio, 0.01, 100.0));\n    if (context.InputDouble("Water Drag / s##volume-output-water-drag", waterDrag))\n        world.Commands().SetProperty(*volumeId, world_model::kVolumeParticleWaterDrag, std::clamp(waterDrag, 0.0, 1000.0));\n    if (context.InputDouble("Buoyancy Scale##volume-output-water-buoyancy", waterBuoyancy))\n        world.Commands().SetProperty(*volumeId, world_model::kVolumeParticleWaterBuoyancyScale, std::clamp(waterBuoyancy, 0.0, 16.0));\n    if (context.Checkbox("Kill On Water Immersion##volume-output-water-kill", killOnWater))\n        world.Commands().SetProperty(*volumeId, world_model::kVolumeParticleKillOnWaterImmersion, killOnWater);\n    if (context.Checkbox("Splash On Water Entry##volume-output-water-splash", splashOnWaterEntry))\n        world.Commands().SetProperty(*volumeId, world_model::kVolumeParticleSplashOnWaterEntry, splashOnWaterEntry);\n    if (context.InputDouble("Splash Scale##volume-output-water-splash-scale", waterSplashScale))\n        world.Commands().SetProperty(*volumeId, world_model::kVolumeParticleWaterSplashScale, std::clamp(waterSplashScale, 0.0, 64.0));\n    context.MutedText("Density ratio 1.0 is neutrally buoyant at full immersion. Water entry is latched on GPU for the splash-output pass; no particle-state CPU readback is used.");\n\n    f64 depositRate = volume->outputSurfaceDepositRatePerSecond;\n''')
replace_once(path,
'''    context.MutedText("Gravity/collision policies are authored and carried per particle. GPU body-gravity and physical-surface response are the next M38 simulation hook.");\n''',
'''    context.MutedText("Gravity, terrain collision and standing-water response are authored per particle and remain GPU-resident. Water-entry events are latched for the GPU splash-output consumer.");\n''')

# Regression: transport + behavior bits.
path='engine/studio_ui/tests/VolumeParticleRenderBridgeTests.cpp'
replace_once(path,
'''            .collisionMode = world_model::VolumeParticleCollisionMode::Bounce,\n            .restitution = 0.6F\n''',
'''            .collisionMode = world_model::VolumeParticleCollisionMode::Bounce,\n            .restitution = 0.6F,\n            .waterDensityRatio = 0.8F,\n            .waterDragPerSecond = 7.5F,\n            .waterBuoyancyScale = 1.2F,\n            .killOnWaterImmersion = true,\n            .splashOnWaterEntry = true,\n            .waterSplashScale = 2.5F\n''')
replace_once(path,
'''    Check(full[0].behaviorFlags == 29U);\n''',
'''    Check(full[0].behaviorFlags == 125U);\n    Check(full[0].waterDensityRatio == 0.8F);\n    Check(full[0].waterDragPerSecond == 7.5F);\n    Check(full[0].waterBuoyancyScale == 1.2F);\n    Check(full[0].waterSplashScale == 2.5F);\n''')

print('M38 authored water behavior patch applied')

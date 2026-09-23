#include <orbit/volume_render/VolumeParticleGpuState.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace orbit::volume_render
{
namespace
{
constexpr const char* kSimulationShader = R"(
struct Spawn
{
    float3 positionMeters;
    float authority;
    float3 velocityMetersPerSecond;
    float density;
    float emission;
    float lifetimeSeconds;
    float linearDragPerSecond;
    float radiusMeters;
    float emissionScale;
    float gravityScale;
    float restitution;
    uint behaviorFlags;
    float3 baseColor;
    float reserved0;
    float3 emissionColor;
    float reserved1;
    float3 bodyCenterMeters;
    float gravitationalParameterM3PerS2;
    float3 surfaceRadiiMeters;
    float gravitySofteningMeters;
    float waterDensityRatio;
    float waterDragPerSecond;
    float waterBuoyancyScale;
    float waterSplashScale;
    uint4 bodyIdentity;
};

struct Particle
{
    float3 positionMeters;
    float authority;
    float3 velocityMetersPerSecond;
    float density;
    float emission;
    float ageSeconds;
    float lifetimeSeconds;
    float linearDragPerSecond;
    float radiusMeters;
    float emissionScale;
    float gravityScale;
    float restitution;
    float3 baseColor;
    uint behaviorFlags;
    float3 emissionColor;
    uint generation;
    float3 bodyCenterMeters;
    float gravitationalParameterM3PerS2;
    float3 surfaceRadiiMeters;
    float gravitySofteningMeters;
    float waterDensityRatio;
    float waterDragPerSecond;
    float waterBuoyancyScale;
    float waterSplashScale;
    uint4 bodyIdentity;
};

[[vk::binding(0, 0)]]
RWStructuredBuffer<Particle> g_source : register(u0);
[[vk::binding(1, 0)]]
RWStructuredBuffer<Particle> g_destination : register(u1);
[[vk::binding(2, 0)]]
RWStructuredBuffer<Spawn> g_spawns : register(u2);
[[vk::binding(3, 0)]]
RWStructuredBuffer<uint> g_counter : register(u3);

struct Push
{
    uint4 counts;
    float4 timing;
    float4 originDelta;
};

[[vk::push_constant]]
Push g;

void AppendParticle(Particle particle)
{
    uint destinationIndex = 0u;
    InterlockedAdd(g_counter[0], 1u, destinationIndex);
    if (destinationIndex < g.counts.w)
    {
        particle.generation = g.counts.y;
        g_destination[destinationIndex] = particle;
    }
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint index = dispatchThreadId.x;
    const uint sourceGeneration = g.counts.x;
    const uint spawnCount = g.counts.z;
    const uint capacity = g.counts.w;
    const float dt = max(g.timing.x, 0.0);
    const float fallbackLifetime = max(g.timing.y, 0.001);
    const float fallbackDrag = max(g.timing.z, 0.0);

    if (index < capacity)
    {
        Particle particle = g_source[index];
        if (particle.generation == sourceGeneration &&
            particle.lifetimeSeconds > 0.0)
        {
            particle.ageSeconds += dt;
            if (particle.ageSeconds < particle.lifetimeSeconds)
            {
                particle.positionMeters += g.originDelta.xyz;
                particle.bodyCenterMeters += g.originDelta.xyz;

                const uint gravityMode = particle.behaviorFlags & 0x3u;
                if (gravityMode == 1u &&
                    particle.gravitationalParameterM3PerS2 > 0.0 &&
                    particle.gravityScale > 0.0)
                {
                    const float3 radial = particle.positionMeters - particle.bodyCenterMeters;
                    const float softening = max(particle.gravitySofteningMeters, 0.0);
                    const float radius2 = dot(radial, radial) + softening * softening;
                    if (radius2 > 1.0e-6)
                    {
                        const float inverseRadius = rsqrt(radius2);
                        const float inverseRadius3 = inverseRadius * inverseRadius * inverseRadius;
                        particle.velocityMetersPerSecond +=
                            -radial * particle.gravitationalParameterM3PerS2 *
                            particle.gravityScale * inverseRadius3 * dt;
                    }
                }

                const float dragScale = exp(-max(particle.linearDragPerSecond, 0.0) * dt);
                particle.velocityMetersPerSecond *= dragScale;
                particle.positionMeters += particle.velocityMetersPerSecond * dt;

                const uint collisionMode = (particle.behaviorFlags >> 2u) & 0x3u;
                bool keepParticle = true;
                if (collisionMode != 0u &&
                    (particle.behaviorFlags & (1u << 4u)) != 0u)
                {
                    const float3 radii = max(particle.surfaceRadiiMeters, 0.001);
                    const float3 local = particle.positionMeters - particle.bodyCenterMeters;
                    const float normalizedRadius2 = dot(local / radii, local / radii);
                    if (normalizedRadius2 <= 1.0)
                    {
                        if (collisionMode == 1u)
                        {
                            keepParticle = false;
                        }
                        else
                        {
                            const float scale = rsqrt(max(normalizedRadius2, 1.0e-12));
                            const float3 surfacePoint = local * scale;
                            const float3 normal = normalize(surfacePoint / (radii * radii));
                            particle.positionMeters =
                                particle.bodyCenterMeters + surfacePoint +
                                normal * max(particle.radiusMeters, 0.001);
                            const float normalSpeed = dot(particle.velocityMetersPerSecond, normal);
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
                        }
                    }
                }

                if (keepParticle)
                {
                    AppendParticle(particle);
                }
            }
        }
    }

    if (index < spawnCount)
    {
        const Spawn spawn = g_spawns[index];
        Particle particle;
        particle.positionMeters = spawn.positionMeters;
        particle.authority = max(spawn.authority, 0.0);
        particle.velocityMetersPerSecond = spawn.velocityMetersPerSecond;
        particle.density = max(spawn.density, 0.0);
        particle.emission = max(spawn.emission, 0.0);
        particle.ageSeconds = 0.0;
        particle.lifetimeSeconds = spawn.lifetimeSeconds > 0.0 ? spawn.lifetimeSeconds : fallbackLifetime;
        particle.linearDragPerSecond = spawn.linearDragPerSecond >= 0.0 ? spawn.linearDragPerSecond : fallbackDrag;
        particle.radiusMeters = max(spawn.radiusMeters, 0.001);
        particle.emissionScale = max(spawn.emissionScale, 0.0);
        particle.gravityScale = max(spawn.gravityScale, 0.0);
        particle.restitution = saturate(spawn.restitution);
        particle.baseColor = max(spawn.baseColor, 0.0);
        particle.behaviorFlags = spawn.behaviorFlags;
        particle.emissionColor = max(spawn.emissionColor, 0.0);
        particle.bodyCenterMeters = spawn.bodyCenterMeters;
        particle.gravitationalParameterM3PerS2 = max(spawn.gravitationalParameterM3PerS2, 0.0);
        particle.surfaceRadiiMeters = max(spawn.surfaceRadiiMeters, 0.0);
        particle.gravitySofteningMeters = max(spawn.gravitySofteningMeters, 0.0);
        particle.waterDensityRatio = max(spawn.waterDensityRatio, 0.01);
        particle.waterDragPerSecond = max(spawn.waterDragPerSecond, 0.0);
        particle.waterBuoyancyScale = max(spawn.waterBuoyancyScale, 0.0);
        particle.waterSplashScale = max(spawn.waterSplashScale, 0.0);
        particle.bodyIdentity = spawn.bodyIdentity;
        particle.generation = g.counts.y;
        AppendParticle(particle);
    }
}
)";



constexpr const char* kTerrainCollisionShader = R"(
struct Particle
{
    float3 positionMeters;
    float authority;
    float3 velocityMetersPerSecond;
    float density;
    float emission;
    float ageSeconds;
    float lifetimeSeconds;
    float linearDragPerSecond;
    float radiusMeters;
    float emissionScale;
    float gravityScale;
    float restitution;
    float3 baseColor;
    uint behaviorFlags;
    float3 emissionColor;
    uint generation;
    float3 bodyCenterMeters;
    float gravitationalParameterM3PerS2;
    float3 surfaceRadiiMeters;
    float gravitySofteningMeters;
    float waterDensityRatio;
    float waterDragPerSecond;
    float waterBuoyancyScale;
    float waterSplashScale;
    uint4 bodyIdentity;
};

struct SplashEvent
{
    float3 positionMeters;
    float scaleMeters;
    float3 normal;
    float impactSpeedMetersPerSecond;
    float3 tint;
    uint generation;
};

[[vk::binding(0, 0)]]
RWStructuredBuffer<Particle> g_particles : register(u0);
[[vk::binding(1, 0)]]
ByteAddressBuffer g_physicalPage : register(t1);
[[vk::binding(2, 0)]]
RWStructuredBuffer<SplashEvent> g_splashes : register(u2);
[[vk::binding(3, 0)]]
RWStructuredBuffer<uint> g_splashCounter : register(u3);

struct Push
{
    uint4 tile;
    uint4 meta;
    uint4 body;
};

[[vk::push_constant]]
Push g;

static const uint kPhysicalTexelStrideBytes = 8u;

void DirectionToCube(float3 direction, out uint face, out float2 uv)
{
    float3 unit = normalize(direction);
    float ax = abs(unit.x);
    float ay = abs(unit.y);
    float az = abs(unit.z);

    if (ax >= ay && ax >= az)
    {
        if (unit.x >= 0.0) { face = 0u; uv = float2(-unit.z / ax, unit.y / ax); }
        else { face = 1u; uv = float2(unit.z / ax, unit.y / ax); }
    }
    else if (ay >= ax && ay >= az)
    {
        if (unit.y >= 0.0) { face = 2u; uv = float2(unit.x / ay, -unit.z / ay); }
        else { face = 3u; uv = float2(unit.x / ay, unit.z / ay); }
    }
    else
    {
        if (unit.z >= 0.0) { face = 4u; uv = float2(unit.x / az, unit.y / az); }
        else { face = 5u; uv = float2(-unit.x / az, unit.y / az); }
    }

    uv = clamp(uv, float2(-1.0, -1.0), float2(1.0, 1.0));
}

uint CoordinateToTileIndex(float coordinate, uint count)
{
    float normalized = clamp(coordinate * 0.5 + 0.5, 0.0, 1.0);
    if (normalized >= 1.0) return count - 1u;
    return uint(normalized * float(count));
}

bool BelongsToPage(uint face, float2 uv)
{
    if (face != g.tile.x) return false;
    uint level = min(g.tile.y, 30u);
    uint count = 1u << level;
    return
        CoordinateToTileIndex(uv.x, count) == g.tile.z &&
        CoordinateToTileIndex(uv.y, count) == g.tile.w;
}

float2 PageBoundsMin()
{
    uint count = 1u << min(g.tile.y, 30u);
    return float2(
        -1.0 + 2.0 * float(g.tile.z) / float(count),
        -1.0 + 2.0 * float(g.tile.w) / float(count));
}

float2 PageBoundsMax()
{
    uint count = 1u << min(g.tile.y, 30u);
    return float2(
        -1.0 + 2.0 * float(g.tile.z + 1u) / float(count),
        -1.0 + 2.0 * float(g.tile.w + 1u) / float(count));
}

float2 LoadPhysicalTexel(uint x, uint y)
{
    uint resolution = g.meta.x;
    uint index = y * resolution + x;
    return asfloat(g_physicalPage.Load2(index * kPhysicalTexelStrideBytes));
}

float2 SamplePhysicalPage(float2 uv)
{
    float2 minimumUv = PageBoundsMin();
    float2 maximumUv = PageBoundsMax();
    float2 extent = max(maximumUv - minimumUv, float2(0.0000001, 0.0000001));
    float2 normalized = saturate((uv - minimumUv) / extent);
    float resolutionMinusOne = float(max(g.meta.x - 1u, 1u));
    float2 coordinate = normalized * resolutionMinusOne;
    uint2 p0 = uint2(floor(coordinate));
    uint2 p1 = min(p0 + uint2(1u, 1u), uint2(g.meta.x - 1u, g.meta.x - 1u));
    float2 fraction = coordinate - float2(p0);
    float2 a = lerp(LoadPhysicalTexel(p0.x, p0.y), LoadPhysicalTexel(p1.x, p0.y), fraction.x);
    float2 b = lerp(LoadPhysicalTexel(p0.x, p1.y), LoadPhysicalTexel(p1.x, p1.y), fraction.x);
    return lerp(a, b, fraction.y);
}

float3 CubeToDirection(uint face, float2 uv)
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

void EmitSplash(Particle particle, float3 positionMeters, float3 normal, float impactSpeed)
{
    const uint kWaterEntryPending = 1u << 30u;
    if ((particle.behaviorFlags & kWaterEntryPending) == 0u) return;

    uint eventIndex = 0u;
    InterlockedAdd(g_splashCounter[0], 1u, eventIndex);
    if (eventIndex < 4096u)
    {
        SplashEvent event;
        event.positionMeters = positionMeters;
        event.scaleMeters = max(particle.radiusMeters * max(particle.waterSplashScale, 0.0), 0.01);
        event.normal = normal;
        event.impactSpeedMetersPerSecond = max(impactSpeed, 0.0);
        event.tint = lerp(float3(1.0, 1.0, 1.0), max(particle.baseColor, 0.0), 0.15);
        event.generation = g.meta.y;
        g_splashes[eventIndex] = event;
    }
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint index = dispatchThreadId.x;
    uint capacity = g.body.z;
    if (index >= capacity) return;

    Particle particle = g_particles[index];
    uint generation = g.meta.y;
    uint4 pageBody = uint4(g.meta.z, g.meta.w, g.body.x, g.body.y);
    uint collisionMode = (particle.behaviorFlags >> 2u) & 0x3u;

    if (particle.generation != generation ||
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

    // Standing-water response is authored per particle. Bit 31 tracks current
    // immersion and bit 30 latches a water-entry event for the GPU splash
    // output pass. No particle-state readback is required.
    bool waterAffected = false;
    bool killForWater = false;
    const uint kWaterEntryPending = 1u << 30u;
    const uint kWaterSubmerged = 1u << 31u;
    bool wasSubmerged = (particle.behaviorFlags & kWaterSubmerged) != 0u;
    bool nowSubmerged = false;
    if (standingWaterDepthMeters > 0.0)
    {
        float waterRadius = terrainRadius + standingWaterDepthMeters;
        float submersion = saturate(
            (waterRadius + particleRadius - radialDistance) /
            max(2.0 * particleRadius, 0.001));
        nowSubmerged = submersion > 0.0;
        if (nowSubmerged)
        {
            float dt = max(asfloat(g.body.w), 0.0);
            float waterDrag = exp(
                -max(particle.waterDragPerSecond, 0.0) * dt * submersion);
            particle.velocityMetersPerSecond *= waterDrag;

            // Main integration already applied gravity. Add only buoyancy here:
            // a density ratio of 1 with scale 1 cancels body gravity at full
            // submersion; ratios below/above 1 rise/sink respectively.
            float3 radial = particle.positionMeters - particle.bodyCenterMeters;
            float radius2 = dot(radial, radial);
            if (radius2 > 1.0e-6 &&
                particle.gravitationalParameterM3PerS2 > 0.0 &&
                particle.gravityScale > 0.0 &&
                particle.waterBuoyancyScale > 0.0)
            {
                float inverseRadius = rsqrt(radius2);
                float gravityAcceleration =
                    particle.gravitationalParameterM3PerS2 / radius2 *
                    particle.gravityScale;
                float buoyancyAcceleration = gravityAcceleration *
                    particle.waterBuoyancyScale /
                    max(particle.waterDensityRatio, 0.01);
                particle.velocityMetersPerSecond +=
                    radial * inverseRadius * buoyancyAcceleration *
                    submersion * dt;
            }

            if (!wasSubmerged &&
                (particle.behaviorFlags & (1u << 6u)) != 0u)
            {
                particle.behaviorFlags |= kWaterEntryPending;
            }

            // Kill only after the particle centre crosses the water surface,
            // avoiding death from a grazing radius contact.
            if ((particle.behaviorFlags & (1u << 5u)) != 0u &&
                radialDistance <= waterRadius)
            {
                killForWater = true;
            }
            waterAffected = true;
        }
    }

    if (nowSubmerged) particle.behaviorFlags |= kWaterSubmerged;
    else particle.behaviorFlags &= ~kWaterSubmerged;

    // Consume the pending entry event exactly once on the first/highest-detail
    // resident page that covers this particle. Later overlapping LOD pages see
    // the cleared bit and cannot duplicate the splash.
    if ((particle.behaviorFlags & kWaterEntryPending) != 0u)
    {
        float waterRadius = terrainRadius + standingWaterDepthMeters;
        float3 waterContact = particle.bodyCenterMeters + direction * waterRadius;
        float impactSpeed = max(-dot(particle.velocityMetersPerSecond, normal), 0.0);
        EmitSplash(particle, waterContact, normal, impactSpeed);
        particle.behaviorFlags &= ~kWaterEntryPending;
    }

    if (killForWater)
    {
        particle.generation = 0u;
        g_particles[index] = particle;
        return;
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

    g_particles[index] = particle;
}
)";

[[nodiscard]] u32 Bits(const f32 value) noexcept
{
    return std::bit_cast<u32>(value);
}
} // namespace

VolumeParticleGpuState::VolumeParticleGpuState(
    rhi::Device& device,
    const shader::Compiler& compiler,
    const u32 framesInFlight)
{
    if (framesInFlight == 0U)
    {
        throw std::invalid_argument(
            "Orbit M38 GPU particle state requires at least one frame in flight.");
    }

    constexpr u64 stateBytes =
        static_cast<u64>(MaximumParticleCount) *
        sizeof(VolumeParticleGpuStateRecord);
    constexpr u64 spawnBytes =
        static_cast<u64>(VolumeParticleGpuBinding::MaximumSpawnCount) *
        sizeof(VolumeParticleGpuSpawn);

    const rhi::BufferDesc stateDesc{
        .sizeBytes = stateBytes,
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::GpuOnly,
        .initialState = rhi::ResourceState::CopyDestination
    };
    stateA_ = device.CreateBuffer(stateDesc);
    stateB_ = device.CreateBuffer(stateDesc);

    zeroStateUpload_ = device.CreateBuffer({
        .sizeBytes = stateBytes,
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::CopySource
    });
    {
        std::byte* mapped = zeroStateUpload_->Map();
        std::memset(mapped, 0, static_cast<std::size_t>(stateBytes));
        zeroStateUpload_->Unmap();
    }

    counter_ = device.CreateBuffer({
        .sizeBytes = sizeof(u32),
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::GpuOnly,
        .initialState = rhi::ResourceState::CopyDestination
    });
    zeroCounterUpload_ = device.CreateBuffer({
        .sizeBytes = sizeof(u32),
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::CopySource
    });
    {
        std::byte* mapped = zeroCounterUpload_->Map();
        std::memset(mapped, 0, sizeof(u32));
        zeroCounterUpload_->Unmap();
    }

    splashEvents_ = device.CreateBuffer({
        .sizeBytes = static_cast<u64>(MaximumSplashEventCount) * sizeof(VolumeParticleGpuSplashEvent),
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::GpuOnly,
        .initialState = rhi::ResourceState::UnorderedAccess
    });
    splashCounter_ = device.CreateBuffer({
        .sizeBytes = sizeof(u32),
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::GpuOnly,
        .initialState = rhi::ResourceState::CopyDestination
    });
    zeroSplashCounterUpload_ = device.CreateBuffer({
        .sizeBytes = sizeof(u32),
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::CopySource
    });
    {
        std::byte* mapped = zeroSplashCounterUpload_->Map();
        std::memset(mapped, 0, sizeof(u32));
        zeroSplashCounterUpload_->Unmap();
    }

    spawnBuffers_.reserve(framesInFlight);
    for (u32 frame = 0U; frame < framesInFlight; ++frame)
    {
        spawnBuffers_.push_back(device.CreateBuffer({
            .sizeBytes = spawnBytes,
            .usage = rhi::BufferUsage::Structured,
            .memory = rhi::MemoryUsage::HostVisible,
            .initialState = rhi::ResourceState::UnorderedAccess
        }));
    }

    const auto compute = compiler.Compile({
        .source = kSimulationShader,
        .entryPoint = "main",
        .stage = shader::Stage::Compute,
        .debug = false
    });
    if (compute.bytecode.empty())
    {
        throw std::runtime_error(
            "Orbit failed to compile the M38 GPU particle simulation shader.");
    }

    simulationPipeline_ = device.CreateComputePipeline({
        .computeShader = {
            .data = compute.bytecode.data(),
            .size = compute.bytecode.size()
        },
        .pushConstantDwords = 12U,
        .shaderResourceBuffers = ComputeBufferCount,
        .storageTextures = 0U,
        .sampledTextures = 0U,
        .accelerationStructures = 0U
    });

    const auto terrainCollision = compiler.Compile({
        .source = kTerrainCollisionShader,
        .entryPoint = "main",
        .stage = shader::Stage::Compute,
        .debug = false
    });
    if (terrainCollision.bytecode.empty())
    {
        throw std::runtime_error(
            "Orbit failed to compile the M38 physical terrain particle collision shader.");
    }

    terrainCollisionPipeline_ = device.CreateComputePipeline({
        .computeShader = {
            .data = terrainCollision.bytecode.data(),
            .size = terrainCollision.bytecode.size()
        },
        .pushConstantDwords = 12U,
        .shaderResourceBuffers = 4U,
        .storageTextures = 0U,
        .sampledTextures = 0U,
        .accelerationStructures = 0U
    });
}

void VolumeParticleGpuState::SetSpawns(
    const std::span<const VolumeParticleGpuSpawn> spawns)
{
    spawnSnapshot_.fill({});
    spawnCount_ = static_cast<u32>(
        std::min<std::size_t>(
            spawns.size(),
            VolumeParticleGpuBinding::MaximumSpawnCount));
    std::copy_n(spawns.begin(), spawnCount_, spawnSnapshot_.begin());
}

void VolumeParticleGpuState::TransitionState(
    rhi::CommandList& commands,
    rhi::Buffer& buffer,
    rhi::ResourceState& tracked,
    const rhi::ResourceState desired)
{
    if (tracked != desired)
    {
        commands.Transition(buffer, tracked, desired);
        tracked = desired;
    }
}

void VolumeParticleGpuState::InitializeState(
    rhi::CommandList& commands)
{
    TransitionState(
        commands, *stateA_, stateAState_, rhi::ResourceState::CopyDestination);
    TransitionState(
        commands, *stateB_, stateBState_, rhi::ResourceState::CopyDestination);
    TransitionState(
        commands, *counter_, counterState_, rhi::ResourceState::CopyDestination);

    commands.CopyBuffer(
        *zeroStateUpload_, 0U, *stateA_, 0U, stateA_->SizeBytes());
    commands.CopyBuffer(
        *zeroStateUpload_, 0U, *stateB_, 0U, stateB_->SizeBytes());
    commands.CopyBuffer(
        *zeroCounterUpload_, 0U, *counter_, 0U, sizeof(u32));

    TransitionState(
        commands, *stateA_, stateAState_, rhi::ResourceState::UnorderedAccess);
    TransitionState(
        commands, *stateB_, stateBState_, rhi::ResourceState::UnorderedAccess);
    TransitionState(
        commands, *counter_, counterState_, rhi::ResourceState::UnorderedAccess);

    currentIsA_ = true;
    generation_ = 0U;
    initialized_ = true;
}

void VolumeParticleGpuState::Advance(
    rhi::CommandList& commands,
    const u32 frameIndex,
    const f64 deltaSeconds,
    const math::Double3 previousOriginMeters,
    const math::Double3 newOriginMeters,
    const VolumeParticleSimulationSettings settings)
{
    if (frameIndex >= spawnBuffers_.size())
    {
        throw std::out_of_range(
            "Orbit M38 particle simulation frame index exceeds frames in flight.");
    }

    if (!initialized_)
    {
        InitializeState(commands);
    }

    rhi::Buffer& spawnBuffer = *spawnBuffers_[frameIndex];
    {
        std::byte* mapped = spawnBuffer.Map();
        std::memcpy(mapped, spawnSnapshot_.data(), sizeof(spawnSnapshot_));
        spawnBuffer.Unmap();
    }

    TransitionState(
        commands, *counter_, counterState_, rhi::ResourceState::CopyDestination);
    commands.CopyBuffer(
        *zeroCounterUpload_, 0U, *counter_, 0U, sizeof(u32));
    TransitionState(
        commands, *counter_, counterState_, rhi::ResourceState::UnorderedAccess);

    rhi::Buffer& source = currentIsA_ ? *stateA_ : *stateB_;
    rhi::Buffer& destination = currentIsA_ ? *stateB_ : *stateA_;
    rhi::ResourceState& sourceState = currentIsA_ ? stateAState_ : stateBState_;
    rhi::ResourceState& destinationState = currentIsA_ ? stateBState_ : stateAState_;

    TransitionState(
        commands, source, sourceState, rhi::ResourceState::UnorderedAccess);
    TransitionState(
        commands, destination, destinationState, rhi::ResourceState::UnorderedAccess);

    const u32 sourceGeneration = generation_;
    ++generation_;
    if (generation_ == 0U)
    {
        generation_ = 1U;
    }

    const f32 dt = std::isfinite(deltaSeconds)
        ? static_cast<f32>(std::clamp(deltaSeconds, 0.0, 1.0))
        : 0.0F;
    const f32 lifetime = std::isfinite(settings.lifetimeSeconds)
        ? std::max(settings.lifetimeSeconds, 0.001F)
        : 2.0F;
    const f32 drag = std::isfinite(settings.linearDragPerSecond)
        ? std::max(settings.linearDragPerSecond, 0.0F)
        : 0.0F;

    const math::Double3 originDelta{
        previousOriginMeters.x - newOriginMeters.x,
        previousOriginMeters.y - newOriginMeters.y,
        previousOriginMeters.z - newOriginMeters.z
    };

    std::array<u32, 12U> constants{};
    constants[0] = sourceGeneration;
    constants[1] = generation_;
    constants[2] = spawnCount_;
    constants[3] = MaximumParticleCount;
    constants[4] = Bits(dt);
    constants[5] = Bits(lifetime);
    constants[6] = Bits(drag);
    constants[8] = Bits(static_cast<f32>(originDelta.x));
    constants[9] = Bits(static_cast<f32>(originDelta.y));
    constants[10] = Bits(static_cast<f32>(originDelta.z));

    commands.SetComputePipeline(*simulationPipeline_);
    commands.SetComputeConstants(constants);
    commands.SetComputeBuffer(0U, source);
    commands.SetComputeBuffer(1U, destination);
    commands.SetComputeBuffer(2U, spawnBuffer);
    commands.SetComputeBuffer(3U, *counter_);
    commands.Dispatch((MaximumParticleCount + 63U) / 64U, 1U, 1U);
    commands.UavBarrier(destination);

    TransitionState(
        commands,
        destination,
        destinationState,
        rhi::ResourceState::ShaderResource);

    currentIsA_ = !currentIsA_;
}


void VolumeParticleGpuState::ApplyTerrainCollision(
    rhi::CommandList& commands,
    const std::span<const VolumeParticleTerrainCollisionPage> pages,
    const f64 deltaSeconds)
{
    if (!initialized_ || generation_ == 0U || pages.empty())
    {
        return;
    }

    rhi::Buffer& current = CurrentBuffer();
    rhi::ResourceState& currentState =
        currentIsA_ ? stateAState_ : stateBState_;

    TransitionState(
        commands,
        current,
        currentState,
        rhi::ResourceState::UnorderedAccess);
    TransitionState(commands, *splashEvents_, splashEventState_, rhi::ResourceState::UnorderedAccess);
    TransitionState(commands, *splashCounter_, splashCounterState_, rhi::ResourceState::CopyDestination);
    commands.CopyBuffer(*zeroSplashCounterUpload_, 0U, *splashCounter_, 0U, sizeof(u32));
    TransitionState(commands, *splashCounter_, splashCounterState_, rhi::ResourceState::UnorderedAccess);

    commands.SetComputePipeline(*terrainCollisionPipeline_);

    for (const auto& page : pages)
    {
        if (!page.IsValid())
        {
            continue;
        }

        std::array<u32, 12U> constants{};
        constants[0] = page.face;
        constants[1] = page.level;
        constants[2] = page.tileX;
        constants[3] = page.tileY;
        constants[4] = page.resolution;
        constants[5] = generation_;
        constants[6] = page.bodyIdentity[0];
        constants[7] = page.bodyIdentity[1];
        constants[8] = page.bodyIdentity[2];
        constants[9] = page.bodyIdentity[3];
        constants[10] = MaximumParticleCount;
        const f32 terrainDeltaSeconds =
            std::isfinite(deltaSeconds)
                ? static_cast<f32>(std::clamp(deltaSeconds, 0.0, 1.0))
                : 0.0F;
        constants[11] = Bits(terrainDeltaSeconds);

        commands.SetComputeConstants(constants);
        commands.SetComputeBuffer(0U, current);
        commands.SetComputeBuffer(1U, *page.samples);
        commands.SetComputeBuffer(2U, *splashEvents_);
        commands.SetComputeBuffer(3U, *splashCounter_);
        commands.Dispatch((MaximumParticleCount + 63U) / 64U, 1U, 1U);
        commands.UavBarrier(current);
        commands.UavBarrier(*splashEvents_);
    }

    TransitionState(
        commands,
        current,
        currentState,
        rhi::ResourceState::ShaderResource);
    TransitionState(commands, *splashEvents_, splashEventState_, rhi::ResourceState::ShaderResource);
}

void VolumeParticleGpuState::BindForGraphics(
    rhi::CommandList& commands)
{
    rhi::Buffer& current = CurrentBuffer();
    rhi::ResourceState& state = currentIsA_ ? stateAState_ : stateBState_;
    TransitionState(
        commands, current, state, rhi::ResourceState::ShaderResource);
    commands.SetGraphicsBuffer(GraphicsBufferSlot, current);
    TransitionState(commands, *splashEvents_, splashEventState_, rhi::ResourceState::ShaderResource);
    commands.SetGraphicsBuffer(SplashGraphicsBufferSlot, *splashEvents_);
}

void VolumeParticleGpuState::Reset() noexcept
{
    spawnSnapshot_.fill({});
    spawnCount_ = 0U;
    generation_ = 0U;
    initialized_ = false;
}

u32 VolumeParticleGpuState::Generation() const noexcept
{
    return generation_;
}

u32 VolumeParticleGpuState::SubmittedSpawnCount() const noexcept
{
    return spawnCount_;
}

rhi::Buffer& VolumeParticleGpuState::CurrentBuffer() noexcept
{
    return currentIsA_ ? *stateA_ : *stateB_;
}
} // namespace orbit::volume_render

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
    float3 bodyCenterMeters;
    float gravitationalParameterM3PerS2;
    float3 surfaceRadiiMeters;
    float gravitySofteningMeters;
    uint4 bodyIdentity;
    uint flags;
    uint3 reserved;
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
        event.bodyCenterMeters = particle.bodyCenterMeters;
        event.gravitationalParameterM3PerS2 = particle.gravitationalParameterM3PerS2;
        event.surfaceRadiiMeters = particle.surfaceRadiiMeters;
        event.gravitySofteningMeters = particle.gravitySofteningMeters;
        event.bodyIdentity = particle.bodyIdentity;
        event.flags = 1u; // Primary particle entry may emit secondary droplets.
        event.reserved = 0u;
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


constexpr const char* kDropletSimulationShader = R"(
struct SplashEvent { float3 positionMeters; float scaleMeters; float3 normal; float impactSpeedMetersPerSecond; float3 tint; uint generation; float3 bodyCenterMeters; float gravitationalParameterM3PerS2; float3 surfaceRadiiMeters; float gravitySofteningMeters; uint4 bodyIdentity; uint flags; uint3 reserved; };
struct Droplet { float3 positionMeters; float radiusMeters; float3 velocityMetersPerSecond; float ageSeconds; float3 tint; float lifetimeSeconds; float3 bodyCenterMeters; float gravitationalParameterM3PerS2; float3 surfaceRadiiMeters; float gravitySofteningMeters; uint4 bodyIdentity; uint generation; uint flags; uint2 reserved; };
[[vk::binding(0,0)]] RWStructuredBuffer<Droplet> g_source : register(u0);
[[vk::binding(1,0)]] RWStructuredBuffer<Droplet> g_destination : register(u1);
[[vk::binding(2,0)]] RWStructuredBuffer<SplashEvent> g_events : register(u2);
[[vk::binding(3,0)]] RWStructuredBuffer<uint> g_eventCounter : register(u3);
[[vk::binding(4,0)]] RWStructuredBuffer<uint> g_counter : register(u4);
struct Push { uint4 counts; float4 timing; };
[[vk::push_constant]] Push g;
void Append(Droplet d){ uint index=0u; InterlockedAdd(g_counter[0],1u,index); if(index<g.counts.w){ d.generation=g.counts.y; g_destination[index]=d; } }
uint Hash(uint x){ x^=x>>16; x*=0x7feb352du; x^=x>>15; x*=0x846ca68bu; x^=x>>16; return x; }
float Unit01(uint x){ return float(Hash(x)&0x00ffffffu)/16777215.0; }
void Basis(float3 n,out float3 t,out float3 b){ float3 a=abs(n.z)<0.9?float3(0,0,1):float3(0,1,0); t=normalize(cross(a,n)); b=normalize(cross(n,t)); }
[numthreads(64,1,1)] void main(uint3 id:SV_DispatchThreadID){
 uint i=id.x; float dt=max(g.timing.x,0.0); float3 originDelta=g.timing.yzw;
 if(i<g.counts.w){ Droplet d=g_source[i]; if(d.generation==g.counts.x&&d.lifetimeSeconds>0.0){ d.ageSeconds+=dt; if(d.ageSeconds<d.lifetimeSeconds){ d.positionMeters+=originDelta; d.bodyCenterMeters+=originDelta; float3 radial=d.positionMeters-d.bodyCenterMeters; float soft=max(d.gravitySofteningMeters,0.0); float r2=dot(radial,radial)+soft*soft; if(r2>1e-6&&d.gravitationalParameterM3PerS2>0.0){ float inv=rsqrt(r2); d.velocityMetersPerSecond+=-radial*d.gravitationalParameterM3PerS2*inv*inv*inv*dt; } d.positionMeters+=d.velocityMetersPerSecond*dt; Append(d); } } }
 uint eventCount=min(g_eventCounter[0],4096u); if(i<eventCount){ SplashEvent e=g_events[i]; if(e.generation==g.counts.z&&(e.flags&1u)!=0u&&e.gravitationalParameterM3PerS2>0.0){ float3 radial=e.positionMeters-e.bodyCenterMeters; float r2=max(dot(radial,radial),1e-6); float gravity=e.gravitationalParameterM3PerS2/r2; float characteristic=sqrt(max(2.0*gravity*max(e.scaleMeters,0.01),0.01)); float energyRatio=e.impactSpeedMetersPerSecond/max(characteristic,0.01); if(energyRatio>1.0){ uint count=min(8u,max(1u,uint(floor(energyRatio)))); float3 n=normalize(e.normal); float3 t,b; Basis(n,t,b); float excess=max(e.impactSpeedMetersPerSecond-characteristic,0.0); [loop] for(uint k=0u;k<count;++k){ float u=Unit01(i*17u+k*131u+g.counts.y*7u); float v=Unit01(i*43u+k*197u+g.counts.y*11u); float angle=6.28318530718*u; float radialMix=sqrt(v); float3 lateral=(cos(angle)*t+sin(angle)*b)*radialMix; float3 dir=normalize(n*(1.0-0.45*radialMix)+lateral*0.45); Droplet d; d.positionMeters=e.positionMeters+n*max(e.scaleMeters*0.04,0.002); d.radiusMeters=max(e.scaleMeters/max(float(count)*12.0,24.0),0.002); d.velocityMetersPerSecond=dir*max(excess,0.25*characteristic); d.ageSeconds=0.0; d.tint=max(e.tint,0.0); d.lifetimeSeconds=clamp(0.5+2.0*length(d.velocityMetersPerSecond)/max(gravity,0.1),0.5,4.0); d.bodyCenterMeters=e.bodyCenterMeters; d.gravitationalParameterM3PerS2=e.gravitationalParameterM3PerS2; d.surfaceRadiiMeters=e.surfaceRadiiMeters; d.gravitySofteningMeters=e.gravitySofteningMeters; d.bodyIdentity=e.bodyIdentity; d.generation=g.counts.y; d.flags=e.generation; d.reserved=0u; Append(d); } } } }
}
)";

constexpr const char* kDropletCollisionShader = R"(
struct Droplet { float3 positionMeters; float radiusMeters; float3 velocityMetersPerSecond; float ageSeconds; float3 tint; float lifetimeSeconds; float3 bodyCenterMeters; float gravitationalParameterM3PerS2; float3 surfaceRadiiMeters; float gravitySofteningMeters; uint4 bodyIdentity; uint generation; uint flags; uint2 reserved; };
struct SplashEvent { float3 positionMeters; float scaleMeters; float3 normal; float impactSpeedMetersPerSecond; float3 tint; uint generation; float3 bodyCenterMeters; float gravitationalParameterM3PerS2; float3 surfaceRadiiMeters; float gravitySofteningMeters; uint4 bodyIdentity; uint flags; uint3 reserved; };
[[vk::binding(0,0)]] RWStructuredBuffer<Droplet> g_droplets : register(u0);
[[vk::binding(1,0)]] ByteAddressBuffer g_physicalPage : register(t1);
[[vk::binding(2,0)]] RWStructuredBuffer<SplashEvent> g_splashes : register(u2);
[[vk::binding(3,0)]] RWStructuredBuffer<uint> g_splashCounter : register(u3);
struct Push { uint4 tile; uint4 meta; uint4 body; };
[[vk::push_constant]] Push g;
static const uint kPhysicalTexelStrideBytes=8u;
void DirectionToCube(float3 d,out uint face,out float2 uv){ float3 u=normalize(d); float ax=abs(u.x),ay=abs(u.y),az=abs(u.z); if(ax>=ay&&ax>=az){if(u.x>=0){face=0;uv=float2(-u.z/ax,u.y/ax);}else{face=1;uv=float2(u.z/ax,u.y/ax);}}else if(ay>=ax&&ay>=az){if(u.y>=0){face=2;uv=float2(u.x/ay,-u.z/ay);}else{face=3;uv=float2(u.x/ay,u.z/ay);}}else{if(u.z>=0){face=4;uv=float2(u.x/az,u.y/az);}else{face=5;uv=float2(-u.x/az,u.y/az);}} uv=clamp(uv,-1.0,1.0); }
uint CoordinateToTileIndex(float c,uint count){ float n=clamp(c*0.5+0.5,0.0,1.0); if(n>=1.0)return count-1u; return uint(n*float(count)); }
bool BelongsToPage(uint face,float2 uv){ if(face!=g.tile.x)return false; uint level=min(g.tile.y,30u),count=1u<<level; return CoordinateToTileIndex(uv.x,count)==g.tile.z&&CoordinateToTileIndex(uv.y,count)==g.tile.w; }
float2 PageBoundsMin(){ uint count=1u<<min(g.tile.y,30u); return float2(-1.0+2.0*float(g.tile.z)/float(count),-1.0+2.0*float(g.tile.w)/float(count)); }
float2 PageBoundsMax(){ uint count=1u<<min(g.tile.y,30u); return float2(-1.0+2.0*float(g.tile.z+1u)/float(count),-1.0+2.0*float(g.tile.w+1u)/float(count)); }
float2 LoadPhysicalTexel(uint x,uint y){ uint index=y*g.meta.x+x; return asfloat(g_physicalPage.Load2(index*kPhysicalTexelStrideBytes)); }
float2 SamplePhysicalPage(float2 uv){ float2 mn=PageBoundsMin(),mx=PageBoundsMax(),extent=max(mx-mn,1e-7); float2 c=saturate((uv-mn)/extent)*float(max(g.meta.x-1u,1u)); uint2 p0=uint2(floor(c)),p1=min(p0+1u,uint2(g.meta.x-1u,g.meta.x-1u)); float2 f=c-float2(p0); float2 a=lerp(LoadPhysicalTexel(p0.x,p0.y),LoadPhysicalTexel(p1.x,p0.y),f.x); float2 b=lerp(LoadPhysicalTexel(p0.x,p1.y),LoadPhysicalTexel(p1.x,p1.y),f.x); return lerp(a,b,f.y); }
float ReferenceRadius(float3 d,float3 r){ float inv=sqrt(dot(d/r,d/r)); return inv>0.0?1.0/inv:0.0; }
bool SameBody(uint4 a,uint4 b){ return all(a==b); }
void EmitChildSplash(Droplet d,float3 position,float3 normal,float speed){ uint index=0u; InterlockedAdd(g_splashCounter[0],1u,index); if(index<4096u){ SplashEvent e; e.positionMeters=position; e.scaleMeters=max(d.radiusMeters*6.0,0.01); e.normal=normal; e.impactSpeedMetersPerSecond=max(speed,0.0); e.tint=d.tint; e.generation=d.flags; e.bodyCenterMeters=d.bodyCenterMeters; e.gravitationalParameterM3PerS2=d.gravitationalParameterM3PerS2; e.surfaceRadiiMeters=d.surfaceRadiiMeters; e.gravitySofteningMeters=d.gravitySofteningMeters; e.bodyIdentity=d.bodyIdentity; e.flags=0u; e.reserved=0u; g_splashes[index]=e; } }
[numthreads(64,1,1)] void main(uint3 id:SV_DispatchThreadID){ uint i=id.x; if(i>=g.body.z)return; Droplet d=g_droplets[i]; if(d.generation!=g.meta.y||!SameBody(d.bodyIdentity,uint4(g.meta.z,g.meta.w,g.body.x,g.body.y)))return; float3 local=d.positionMeters-d.bodyCenterMeters; float dist=length(local); if(dist<=1e-6)return; float3 dir=local/dist; uint face; float2 uv; DirectionToCube(dir,face,uv); if(!BelongsToPage(face,uv))return; float2 physical=SamplePhysicalPage(uv); float reference=ReferenceRadius(dir,max(d.surfaceRadiiMeters,0.001)); if(reference<=0.0)return; float terrain=reference+physical.x; float water=terrain+max(physical.y,0.0); float previousDist=length((d.positionMeters-d.velocityMetersPerSecond*max(asfloat(g.body.w),0.0))-d.bodyCenterMeters); if(physical.y>0.0&&previousDist>water&&dist<=water+d.radiusMeters){ float3 n=dir; float speed=max(-dot(d.velocityMetersPerSecond,n),0.0); EmitChildSplash(d,d.bodyCenterMeters+dir*water,n,speed); d.generation=0u; g_droplets[i]=d; return; } if(dist<=terrain+d.radiusMeters){ d.generation=0u; g_droplets[i]=d; } }
)";

constexpr const char* kSplashSimulationShader = R"(
struct SplashEvent { float3 positionMeters; float scaleMeters; float3 normal; float impactSpeedMetersPerSecond; float3 tint; uint generation; float3 bodyCenterMeters; float gravitationalParameterM3PerS2; float3 surfaceRadiiMeters; float gravitySofteningMeters; uint4 bodyIdentity; uint flags; uint3 reserved; };
struct SplashState { float3 positionMeters; float baseScaleMeters; float3 normal; float expansionMetersPerSecond; float3 tint; float impactSpeedMetersPerSecond; float ageSeconds; float lifetimeSeconds; uint generation; uint reserved; };
[[vk::binding(0,0)]] RWStructuredBuffer<SplashState> g_source : register(u0);
[[vk::binding(1,0)]] RWStructuredBuffer<SplashState> g_destination : register(u1);
[[vk::binding(2,0)]] RWStructuredBuffer<SplashEvent> g_events : register(u2);
[[vk::binding(3,0)]] RWStructuredBuffer<uint> g_eventCounter : register(u3);
[[vk::binding(4,0)]] RWStructuredBuffer<uint> g_stateCounter : register(u4);
struct Push { uint4 counts; float4 timing; };
[[vk::push_constant]] Push g;
void Append(SplashState s){ uint i=0u; InterlockedAdd(g_stateCounter[0],1u,i); if(i<g.counts.w){ s.generation=g.counts.y; g_destination[i]=s; } }
[numthreads(64,1,1)] void main(uint3 id:SV_DispatchThreadID){
 uint i=id.x; float dt=max(g.timing.x,0.0); float3 originDelta=g.timing.yzw;
 if(i<g.counts.w){ SplashState s=g_source[i]; if(s.generation==g.counts.x && s.lifetimeSeconds>0.0){ s.ageSeconds+=dt; if(s.ageSeconds<s.lifetimeSeconds){ s.positionMeters+=originDelta; Append(s); } } }
 uint eventCount=min(g_eventCounter[0],4096u);
 if(i<eventCount){ SplashEvent e=g_events[i]; if(e.generation==g.counts.z){ SplashState s; s.positionMeters=e.positionMeters; s.baseScaleMeters=max(e.scaleMeters,0.01); s.normal=normalize(e.normal); s.expansionMetersPerSecond=max(0.35*e.impactSpeedMetersPerSecond,0.15*s.baseScaleMeters); s.tint=max(e.tint,0.0); s.impactSpeedMetersPerSecond=max(e.impactSpeedMetersPerSecond,0.0); s.ageSeconds=0.0; s.lifetimeSeconds=clamp(0.45+0.10*s.impactSpeedMetersPerSecond,0.45,1.75); s.generation=g.counts.y; s.reserved=0u; Append(s); } }
}
)";


constexpr const char* kDrawListShader = R"(
struct Particle { float3 positionMeters; float authority; float3 velocityMetersPerSecond; float density; float emission; float ageSeconds; float lifetimeSeconds; float linearDragPerSecond; float radiusMeters; float emissionScale; float gravityScale; float restitution; float3 baseColor; uint behaviorFlags; float3 emissionColor; uint generation; float3 bodyCenterMeters; float gravitationalParameterM3PerS2; float3 surfaceRadiiMeters; float gravitySofteningMeters; float waterDensityRatio; float waterDragPerSecond; float waterBuoyancyScale; float waterSplashScale; uint4 bodyIdentity; };
struct SplashState { float3 positionMeters; float baseScaleMeters; float3 normal; float expansionMetersPerSecond; float3 tint; float impactSpeedMetersPerSecond; float ageSeconds; float lifetimeSeconds; uint generation; uint reserved; };
struct Droplet { float3 positionMeters; float radiusMeters; float3 velocityMetersPerSecond; float ageSeconds; float3 tint; float lifetimeSeconds; float3 bodyCenterMeters; float gravitationalParameterM3PerS2; float3 surfaceRadiiMeters; float gravitySofteningMeters; uint4 bodyIdentity; uint generation; uint flags; uint sourceParticleGeneration; uint reserved1; };
[[vk::binding(0,0)]] StructuredBuffer<Particle> g_particles : register(t0);
[[vk::binding(1,0)]] StructuredBuffer<SplashState> g_splashes : register(t1);
[[vk::binding(2,0)]] StructuredBuffer<Droplet> g_droplets : register(t2);
[[vk::binding(3,0)]] RWStructuredBuffer<uint> g_particleIndices : register(u3);
[[vk::binding(4,0)]] RWStructuredBuffer<uint> g_splashIndices : register(u4);
[[vk::binding(5,0)]] RWStructuredBuffer<uint> g_dropletIndices : register(u5);
[[vk::binding(6,0)]] RWByteAddressBuffer g_drawArgs : register(u6);
struct Push { uint4 generation; uint4 capacity; }; [[vk::push_constant]] Push g;
void AppendIndex(uint argsOffset,uint index,RWStructuredBuffer<uint> indices){ uint oldVertices=0u; g_drawArgs.InterlockedAdd(argsOffset,6u,oldVertices); indices[oldVertices/6u]=index; }
[numthreads(64,1,1)] void main(uint3 id:SV_DispatchThreadID){ uint i=id.x;
 if(i<g.capacity.x){ Particle p=g_particles[i]; if(p.generation==g.generation.x&&p.lifetimeSeconds>0.0&&p.ageSeconds<p.lifetimeSeconds) AppendIndex(0u,i,g_particleIndices); }
 if(i<g.capacity.y){ SplashState s=g_splashes[i]; if(s.generation==g.generation.y&&s.lifetimeSeconds>0.0&&s.ageSeconds<s.lifetimeSeconds) AppendIndex(16u,i,g_splashIndices); }
 if(i<g.capacity.z){ Droplet d=g_droplets[i]; if(d.generation==g.generation.z&&d.lifetimeSeconds>0.0&&d.ageSeconds<d.lifetimeSeconds) AppendIndex(32u,i,g_dropletIndices); }
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

    const u64 splashStateBytes = static_cast<u64>(MaximumPersistentSplashCount) * sizeof(VolumeParticleGpuSplashState);
    const rhi::BufferDesc splashStateDesc{.sizeBytes=splashStateBytes,.usage=rhi::BufferUsage::Structured,.memory=rhi::MemoryUsage::GpuOnly,.initialState=rhi::ResourceState::CopyDestination};
    splashStateA_=device.CreateBuffer(splashStateDesc);
    splashStateB_=device.CreateBuffer(splashStateDesc);
    zeroSplashStateUpload_=device.CreateBuffer({.sizeBytes=splashStateBytes,.usage=rhi::BufferUsage::Structured,.memory=rhi::MemoryUsage::HostVisible,.initialState=rhi::ResourceState::CopySource});
    { std::byte* mapped=zeroSplashStateUpload_->Map(); std::memset(mapped,0,static_cast<std::size_t>(splashStateBytes)); zeroSplashStateUpload_->Unmap(); }
    splashStateCounter_=device.CreateBuffer({.sizeBytes=sizeof(u32),.usage=rhi::BufferUsage::Structured,.memory=rhi::MemoryUsage::GpuOnly,.initialState=rhi::ResourceState::CopyDestination});
    zeroSplashStateCounterUpload_=device.CreateBuffer({.sizeBytes=sizeof(u32),.usage=rhi::BufferUsage::Structured,.memory=rhi::MemoryUsage::HostVisible,.initialState=rhi::ResourceState::CopySource});
    { std::byte* mapped=zeroSplashStateCounterUpload_->Map(); std::memset(mapped,0,sizeof(u32)); zeroSplashStateCounterUpload_->Unmap(); }

    const u64 dropletBytes=static_cast<u64>(MaximumDropletCount)*sizeof(VolumeParticleGpuDropletState);
    const rhi::BufferDesc dropletDesc{.sizeBytes=dropletBytes,.usage=rhi::BufferUsage::Structured,.memory=rhi::MemoryUsage::GpuOnly,.initialState=rhi::ResourceState::CopyDestination};
    dropletStateA_=device.CreateBuffer(dropletDesc); dropletStateB_=device.CreateBuffer(dropletDesc);
    zeroDropletStateUpload_=device.CreateBuffer({.sizeBytes=dropletBytes,.usage=rhi::BufferUsage::Structured,.memory=rhi::MemoryUsage::HostVisible,.initialState=rhi::ResourceState::CopySource});
    { std::byte* mapped=zeroDropletStateUpload_->Map(); std::memset(mapped,0,static_cast<std::size_t>(dropletBytes)); zeroDropletStateUpload_->Unmap(); }
    dropletCounter_=device.CreateBuffer({.sizeBytes=sizeof(u32),.usage=rhi::BufferUsage::Structured,.memory=rhi::MemoryUsage::GpuOnly,.initialState=rhi::ResourceState::CopyDestination});
    zeroDropletCounterUpload_=device.CreateBuffer({.sizeBytes=sizeof(u32),.usage=rhi::BufferUsage::Structured,.memory=rhi::MemoryUsage::HostVisible,.initialState=rhi::ResourceState::CopySource});
    { std::byte* mapped=zeroDropletCounterUpload_->Map(); std::memset(mapped,0,sizeof(u32)); zeroDropletCounterUpload_->Unmap(); }

    particleActiveIndices_=device.CreateBuffer({.sizeBytes=static_cast<u64>(MaximumParticleCount)*sizeof(u32),.usage=rhi::BufferUsage::Structured,.memory=rhi::MemoryUsage::GpuOnly,.initialState=rhi::ResourceState::UnorderedAccess});
    splashActiveIndices_=device.CreateBuffer({.sizeBytes=static_cast<u64>(MaximumPersistentSplashCount)*sizeof(u32),.usage=rhi::BufferUsage::Structured,.memory=rhi::MemoryUsage::GpuOnly,.initialState=rhi::ResourceState::UnorderedAccess});
    dropletActiveIndices_=device.CreateBuffer({.sizeBytes=static_cast<u64>(MaximumDropletCount)*sizeof(u32),.usage=rhi::BufferUsage::Structured,.memory=rhi::MemoryUsage::GpuOnly,.initialState=rhi::ResourceState::UnorderedAccess});
    indirectDrawArguments_=device.CreateBuffer({.sizeBytes=48U,.usage=rhi::BufferUsage::Indirect,.memory=rhi::MemoryUsage::GpuOnly,.initialState=rhi::ResourceState::CopyDestination});
    zeroIndirectDrawArgumentsUpload_=device.CreateBuffer({.sizeBytes=48U,.usage=rhi::BufferUsage::Indirect,.memory=rhi::MemoryUsage::HostVisible,.initialState=rhi::ResourceState::CopySource});
    { auto* mapped=reinterpret_cast<u32*>(zeroIndirectDrawArgumentsUpload_->Map()); std::memset(mapped,0,48U); mapped[1]=1U; mapped[5]=1U; mapped[9]=1U; zeroIndirectDrawArgumentsUpload_->Unmap(); }

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

    const auto splashSimulation=compiler.Compile({.source=kSplashSimulationShader,.entryPoint="main",.stage=shader::Stage::Compute,.debug=false});
    if(splashSimulation.bytecode.empty()) throw std::runtime_error("Orbit failed to compile the M38 persistent splash simulation shader.");
    splashSimulationPipeline_=device.CreateComputePipeline({.computeShader={.data=splashSimulation.bytecode.data(),.size=splashSimulation.bytecode.size()},.pushConstantDwords=8U,.shaderResourceBuffers=5U,.storageTextures=0U,.sampledTextures=0U,.accelerationStructures=0U});
    const auto dropletSimulation=compiler.Compile({.source=kDropletSimulationShader,.entryPoint="main",.stage=shader::Stage::Compute,.debug=false});
    const auto dropletCollision=compiler.Compile({.source=kDropletCollisionShader,.entryPoint="main",.stage=shader::Stage::Compute,.debug=false});
    if(dropletSimulation.bytecode.empty()||dropletCollision.bytecode.empty()) throw std::runtime_error("Orbit failed to compile the M38 secondary droplet shaders.");
    dropletSimulationPipeline_=device.CreateComputePipeline({.computeShader={.data=dropletSimulation.bytecode.data(),.size=dropletSimulation.bytecode.size()},.pushConstantDwords=8U,.shaderResourceBuffers=5U,.storageTextures=0U,.sampledTextures=0U,.accelerationStructures=0U});
    dropletCollisionPipeline_=device.CreateComputePipeline({.computeShader={.data=dropletCollision.bytecode.data(),.size=dropletCollision.bytecode.size()},.pushConstantDwords=12U,.shaderResourceBuffers=4U,.storageTextures=0U,.sampledTextures=0U,.accelerationStructures=0U});
    const auto drawList=compiler.Compile({.source=kDrawListShader,.entryPoint="main",.stage=shader::Stage::Compute,.debug=false});
    if(drawList.bytecode.empty()) throw std::runtime_error("Orbit failed to compile the M38 GPU draw-list shader.");
    drawListPipeline_=device.CreateComputePipeline({.computeShader={.data=drawList.bytecode.data(),.size=drawList.bytecode.size()},.pushConstantDwords=8U,.shaderResourceBuffers=7U,.storageTextures=0U,.sampledTextures=0U,.accelerationStructures=0U});
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
    TransitionState(commands,*splashStateA_,splashStateAState_,rhi::ResourceState::CopyDestination);
    TransitionState(commands,*splashStateB_,splashStateBState_,rhi::ResourceState::CopyDestination);
    TransitionState(commands,*splashStateCounter_,splashStateCounterState_,rhi::ResourceState::CopyDestination);
    commands.CopyBuffer(*zeroSplashStateUpload_,0U,*splashStateA_,0U,splashStateA_->SizeBytes());
    commands.CopyBuffer(*zeroSplashStateUpload_,0U,*splashStateB_,0U,splashStateB_->SizeBytes());
    commands.CopyBuffer(*zeroSplashStateCounterUpload_,0U,*splashStateCounter_,0U,sizeof(u32));
    TransitionState(commands,*dropletStateA_,dropletStateAState_,rhi::ResourceState::CopyDestination);
    TransitionState(commands,*dropletStateB_,dropletStateBState_,rhi::ResourceState::CopyDestination);
    TransitionState(commands,*dropletCounter_,dropletCounterState_,rhi::ResourceState::CopyDestination);
    commands.CopyBuffer(*zeroDropletStateUpload_,0U,*dropletStateA_,0U,dropletStateA_->SizeBytes());
    commands.CopyBuffer(*zeroDropletStateUpload_,0U,*dropletStateB_,0U,dropletStateB_->SizeBytes());
    commands.CopyBuffer(*zeroDropletCounterUpload_,0U,*dropletCounter_,0U,sizeof(u32));

    TransitionState(
        commands, *stateA_, stateAState_, rhi::ResourceState::UnorderedAccess);
    TransitionState(
        commands, *stateB_, stateBState_, rhi::ResourceState::UnorderedAccess);
    TransitionState(
        commands, *counter_, counterState_, rhi::ResourceState::UnorderedAccess);

    currentIsA_ = true;
    splashCurrentIsA_ = true;
    dropletCurrentIsA_ = true;
    generation_ = 0U;
    splashGeneration_ = 0U;
    dropletGeneration_ = 0U;
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
    splashOriginDeltaMeters_ = originDelta;

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
    if (!initialized_ || generation_ == 0U)
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
    TransitionState(commands, *splashEvents_, splashEventState_, rhi::ResourceState::UnorderedAccess);

    // Advance/compact ballistic droplets and spawn a bounded burst only from
    // primary splash events (event flags bit 0). Child re-entry splashes clear
    // that bit, preventing recursive spray explosions.
    rhi::Buffer& dropletSource=dropletCurrentIsA_?*dropletStateA_:*dropletStateB_;
    rhi::Buffer& dropletDestination=dropletCurrentIsA_?*dropletStateB_:*dropletStateA_;
    rhi::ResourceState& dropletSourceState=dropletCurrentIsA_?dropletStateAState_:dropletStateBState_;
    rhi::ResourceState& dropletDestinationState=dropletCurrentIsA_?dropletStateBState_:dropletStateAState_;
    TransitionState(commands,dropletSource,dropletSourceState,rhi::ResourceState::UnorderedAccess);
    TransitionState(commands,dropletDestination,dropletDestinationState,rhi::ResourceState::UnorderedAccess);
    TransitionState(commands,*dropletCounter_,dropletCounterState_,rhi::ResourceState::CopyDestination);
    commands.CopyBuffer(*zeroDropletCounterUpload_,0U,*dropletCounter_,0U,sizeof(u32));
    TransitionState(commands,*dropletCounter_,dropletCounterState_,rhi::ResourceState::UnorderedAccess);
    const u32 previousDropletGeneration=dropletGeneration_; ++dropletGeneration_; if(dropletGeneration_==0U) dropletGeneration_=1U;
    const f32 dropletDt=std::isfinite(deltaSeconds)?static_cast<f32>(std::clamp(deltaSeconds,0.0,1.0)):0.0F;
    std::array<u32,8U> dropletConstants{}; dropletConstants[0]=previousDropletGeneration; dropletConstants[1]=dropletGeneration_; dropletConstants[2]=generation_; dropletConstants[3]=MaximumDropletCount; dropletConstants[4]=Bits(dropletDt); dropletConstants[5]=Bits(static_cast<f32>(splashOriginDeltaMeters_.x)); dropletConstants[6]=Bits(static_cast<f32>(splashOriginDeltaMeters_.y)); dropletConstants[7]=Bits(static_cast<f32>(splashOriginDeltaMeters_.z));
    TransitionState(commands,*splashEvents_,splashEventState_,rhi::ResourceState::UnorderedAccess);
    commands.SetComputePipeline(*dropletSimulationPipeline_); commands.SetComputeConstants(dropletConstants); commands.SetComputeBuffer(0U,dropletSource); commands.SetComputeBuffer(1U,dropletDestination); commands.SetComputeBuffer(2U,*splashEvents_); commands.SetComputeBuffer(3U,*splashCounter_); commands.SetComputeBuffer(4U,*dropletCounter_);
    commands.Dispatch((std::max(MaximumDropletCount,MaximumSplashEventCount)+63U)/64U,1U,1U); commands.UavBarrier(dropletDestination); dropletCurrentIsA_=!dropletCurrentIsA_;

    // Refine droplet contact against the same resident physical pages. Water
    // re-entry appends a child splash event with allowSpray=false; terrain-only
    // contact simply retires the droplet.
    commands.SetComputePipeline(*dropletCollisionPipeline_);
    for(const auto& page:pages){ if(!page.IsValid()) continue; std::array<u32,12U> c{}; c[0]=page.face;c[1]=page.level;c[2]=page.tileX;c[3]=page.tileY;c[4]=page.resolution;c[5]=dropletGeneration_;c[6]=page.bodyIdentity[0];c[7]=page.bodyIdentity[1];c[8]=page.bodyIdentity[2];c[9]=page.bodyIdentity[3];c[10]=MaximumDropletCount;c[11]=Bits(dropletDt); commands.SetComputeConstants(c);commands.SetComputeBuffer(0U,dropletDestination);commands.SetComputeBuffer(1U,*page.samples);commands.SetComputeBuffer(2U,*splashEvents_);commands.SetComputeBuffer(3U,*splashCounter_);commands.Dispatch((MaximumDropletCount+63U)/64U,1U,1U);commands.UavBarrier(dropletDestination);commands.UavBarrier(*splashEvents_); }
    TransitionState(commands,dropletDestination,dropletDestinationState,rhi::ResourceState::ShaderResource);

    // Compact/age persistent splash state and append this generation's exact-once water-entry events.
    rhi::Buffer& splashSource=splashCurrentIsA_?*splashStateA_:*splashStateB_;
    rhi::Buffer& splashDestination=splashCurrentIsA_?*splashStateB_:*splashStateA_;
    rhi::ResourceState& splashSourceState=splashCurrentIsA_?splashStateAState_:splashStateBState_;
    rhi::ResourceState& splashDestinationState=splashCurrentIsA_?splashStateBState_:splashStateAState_;
    TransitionState(commands,splashSource,splashSourceState,rhi::ResourceState::UnorderedAccess);
    TransitionState(commands,splashDestination,splashDestinationState,rhi::ResourceState::UnorderedAccess);
    TransitionState(commands,*splashStateCounter_,splashStateCounterState_,rhi::ResourceState::CopyDestination);
    commands.CopyBuffer(*zeroSplashStateCounterUpload_,0U,*splashStateCounter_,0U,sizeof(u32));
    TransitionState(commands,*splashStateCounter_,splashStateCounterState_,rhi::ResourceState::UnorderedAccess);
    const u32 previousSplashGeneration=splashGeneration_; ++splashGeneration_; if(splashGeneration_==0U) splashGeneration_=1U;
    const f32 splashDt=std::isfinite(deltaSeconds)?static_cast<f32>(std::clamp(deltaSeconds,0.0,1.0)):0.0F;
    std::array<u32,8U> splashConstants{}; splashConstants[0]=previousSplashGeneration; splashConstants[1]=splashGeneration_; splashConstants[2]=generation_; splashConstants[3]=MaximumPersistentSplashCount; splashConstants[4]=Bits(splashDt);
    // New events are already in the new presentation frame. Only surviving splash state is rebased.
    splashConstants[5]=Bits(static_cast<f32>(splashOriginDeltaMeters_.x)); splashConstants[6]=Bits(static_cast<f32>(splashOriginDeltaMeters_.y)); splashConstants[7]=Bits(static_cast<f32>(splashOriginDeltaMeters_.z));
    commands.SetComputePipeline(*splashSimulationPipeline_); commands.SetComputeConstants(splashConstants); commands.SetComputeBuffer(0U,splashSource); commands.SetComputeBuffer(1U,splashDestination); commands.SetComputeBuffer(2U,*splashEvents_); commands.SetComputeBuffer(3U,*splashCounter_); commands.SetComputeBuffer(4U,*splashStateCounter_);
    commands.Dispatch((std::max(MaximumPersistentSplashCount,MaximumSplashEventCount)+63U)/64U,1U,1U); commands.UavBarrier(splashDestination); splashCurrentIsA_=!splashCurrentIsA_;
    splashOriginDeltaMeters_ = {};
    TransitionState(commands,splashDestination,splashDestinationState,rhi::ResourceState::ShaderResource);

    // Build compact GPU draw lists after all in-place collision kills. This
    // avoids assuming the surviving states are still dense.
    TransitionState(commands,*indirectDrawArguments_,indirectDrawArgumentsState_,rhi::ResourceState::CopyDestination);
    commands.CopyBuffer(*zeroIndirectDrawArgumentsUpload_,0U,*indirectDrawArguments_,0U,48U);
    TransitionState(commands,*indirectDrawArguments_,indirectDrawArgumentsState_,rhi::ResourceState::UnorderedAccess);
    TransitionState(commands,*particleActiveIndices_,particleActiveIndicesState_,rhi::ResourceState::UnorderedAccess);
    TransitionState(commands,*splashActiveIndices_,splashActiveIndicesState_,rhi::ResourceState::UnorderedAccess);
    TransitionState(commands,*dropletActiveIndices_,dropletActiveIndicesState_,rhi::ResourceState::UnorderedAccess);
    std::array<u32,8U> drawConstants{}; drawConstants[0]=generation_; drawConstants[1]=splashGeneration_; drawConstants[2]=dropletGeneration_; drawConstants[4]=MaximumParticleCount; drawConstants[5]=MaximumPersistentSplashCount; drawConstants[6]=MaximumDropletCount;
    commands.SetComputePipeline(*drawListPipeline_); commands.SetComputeConstants(drawConstants);
    commands.SetComputeBuffer(0U,current); commands.SetComputeBuffer(1U,splashDestination); commands.SetComputeBuffer(2U,dropletDestination); commands.SetComputeBuffer(3U,*particleActiveIndices_); commands.SetComputeBuffer(4U,*splashActiveIndices_); commands.SetComputeBuffer(5U,*dropletActiveIndices_); commands.SetComputeBuffer(6U,*indirectDrawArguments_);
    commands.Dispatch((MaximumParticleCount+63U)/64U,1U,1U);
    commands.UavBarrier(*particleActiveIndices_); commands.UavBarrier(*splashActiveIndices_); commands.UavBarrier(*dropletActiveIndices_); commands.UavBarrier(*indirectDrawArguments_);
    TransitionState(commands,*particleActiveIndices_,particleActiveIndicesState_,rhi::ResourceState::ShaderResource);
    TransitionState(commands,*splashActiveIndices_,splashActiveIndicesState_,rhi::ResourceState::ShaderResource);
    TransitionState(commands,*dropletActiveIndices_,dropletActiveIndicesState_,rhi::ResourceState::ShaderResource);
    TransitionState(commands,*indirectDrawArguments_,indirectDrawArgumentsState_,rhi::ResourceState::IndirectArgument);
}

void VolumeParticleGpuState::BindForGraphics(
    rhi::CommandList& commands)
{
    rhi::Buffer& current = CurrentBuffer();
    rhi::ResourceState& state = currentIsA_ ? stateAState_ : stateBState_;
    TransitionState(
        commands, current, state, rhi::ResourceState::ShaderResource);
    commands.SetGraphicsBuffer(GraphicsBufferSlot, current);
    rhi::Buffer& splashCurrent=splashCurrentIsA_?*splashStateA_:*splashStateB_;
    rhi::ResourceState& splashState=splashCurrentIsA_?splashStateAState_:splashStateBState_;
    TransitionState(commands,splashCurrent,splashState,rhi::ResourceState::ShaderResource);
    commands.SetGraphicsBuffer(SplashGraphicsBufferSlot,splashCurrent);
    rhi::Buffer& dropletCurrent=dropletCurrentIsA_?*dropletStateA_:*dropletStateB_;
    rhi::ResourceState& dropletState=dropletCurrentIsA_?dropletStateAState_:dropletStateBState_;
    TransitionState(commands,dropletCurrent,dropletState,rhi::ResourceState::ShaderResource);
    commands.SetGraphicsBuffer(DropletGraphicsBufferSlot,dropletCurrent);
    TransitionState(commands,*particleActiveIndices_,particleActiveIndicesState_,rhi::ResourceState::ShaderResource);
    TransitionState(commands,*splashActiveIndices_,splashActiveIndicesState_,rhi::ResourceState::ShaderResource);
    TransitionState(commands,*dropletActiveIndices_,dropletActiveIndicesState_,rhi::ResourceState::ShaderResource);
    commands.SetGraphicsBuffer(ParticleActiveIndexGraphicsBufferSlot,*particleActiveIndices_);
    commands.SetGraphicsBuffer(SplashActiveIndexGraphicsBufferSlot,*splashActiveIndices_);
    commands.SetGraphicsBuffer(DropletActiveIndexGraphicsBufferSlot,*dropletActiveIndices_);
}

void VolumeParticleGpuState::Reset() noexcept
{
    spawnSnapshot_.fill({});
    spawnCount_ = 0U;
    generation_ = 0U;
    splashGeneration_ = 0U;
    dropletGeneration_ = 0U;
    splashCurrentIsA_ = true;
    dropletCurrentIsA_ = true;
    splashOriginDeltaMeters_ = {};
    initialized_ = false;
}

u32 VolumeParticleGpuState::Generation() const noexcept
{
    return generation_;
}

u32 VolumeParticleGpuState::SplashGeneration() const noexcept
{
    return splashGeneration_;
}

u32 VolumeParticleGpuState::DropletGeneration() const noexcept
{
    return dropletGeneration_;
}

u32 VolumeParticleGpuState::SubmittedSpawnCount() const noexcept
{
    return spawnCount_;
}

rhi::Buffer& VolumeParticleGpuState::CurrentBuffer() noexcept
{
    return currentIsA_ ? *stateA_ : *stateB_;
}

rhi::Buffer& VolumeParticleGpuState::IndirectDrawArguments() noexcept
{
    return *indirectDrawArguments_;
}
} // namespace orbit::volume_render

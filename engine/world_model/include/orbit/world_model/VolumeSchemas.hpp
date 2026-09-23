#pragma once

#include <orbit/math/Vector.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::world_model
{
inline constexpr schema::TypeId kVolumeType{
    .high = 0x4f52424954564f4cULL,
    .low = 0x554d455459504501ULL
};

inline constexpr schema::TypeId kVolumeSourceType{
    .high = 0x4f52424954564f4cULL,
    .low = 0x534f555243450001ULL
};

inline constexpr schema::TypeId kVolumeEffectorType{
    .high = 0x4f52424954564f4cULL,
    .low = 0x4546464543540001ULL
};

inline constexpr schema::PropertyId kVolumeEnabled{
    .high = 0x4f52424954564f4cULL, .low = 0x454e41424c454401ULL};
inline constexpr schema::PropertyId kVolumePreset{
    .high = 0x4f52424954564f4cULL, .low = 0x5052455345540001ULL};
inline constexpr schema::PropertyId kVolumeCenterMeters{
    .high = 0x4f52424954564f4cULL, .low = 0x43454e5445520001ULL};
inline constexpr schema::PropertyId kVolumeHalfExtentsMeters{
    .high = 0x4f52424954564f4cULL, .low = 0x48414c4645585401ULL};
inline constexpr schema::PropertyId kVolumeSolverPolicy{
    .high = 0x4f52424954564f4cULL, .low = 0x534f4c5645520001ULL};
inline constexpr schema::PropertyId kVolumeRepresentationMode{
    .high = 0x4f52424954564f4cULL, .low = 0x5245504d4f444501ULL};
inline constexpr schema::PropertyId kVolumeFieldMask{
    .high = 0x4f52424954564f4cULL, .low = 0x4649454c444d534bULL};
inline constexpr schema::PropertyId kVolumeResolution{
    .high = 0x4f52424954564f4cULL, .low = 0x5245534f4c555401ULL};
inline constexpr schema::PropertyId kVolumeSurfaceLayers{
    .high = 0x4f52424954564f4cULL, .low = 0x535552464c415901ULL};
inline constexpr schema::PropertyId kVolumeRenderEnabled{
    .high = 0x4f52424954564f4cULL, .low = 0x52454e44454e0001ULL};
inline constexpr schema::PropertyId kVolumeExtinctionScale{
    .high = 0x4f52424954564f4cULL, .low = 0x455854494e435401ULL};
inline constexpr schema::PropertyId kVolumeSingleScatteringAlbedo{
    .high = 0x4f52424954564f4cULL, .low = 0x53434154414c4201ULL};
inline constexpr schema::PropertyId kVolumeScatteringColor{
    .high = 0x4f52424954564f4cULL, .low = 0x53434154434f4c01ULL};
inline constexpr schema::PropertyId kVolumeAnisotropy{
    .high = 0x4f52424954564f4cULL, .low = 0x414e49534f545201ULL};
inline constexpr schema::PropertyId kVolumeEmissionColor{
    .high = 0x4f52424954564f4cULL, .low = 0x454d4954434f4c01ULL};
inline constexpr schema::PropertyId kVolumeEmissionScale{
    .high = 0x4f52424954564f4cULL, .low = 0x454d495453434101ULL};
inline constexpr schema::PropertyId kVolumeGiEmissionScale{
    .high = 0x4f52424954564f4cULL, .low = 0x4749454d49545301ULL};
inline constexpr schema::PropertyId kVolumeRenderSteps{
    .high = 0x4f52424954564f4cULL, .low = 0x5241595354455001ULL};
inline constexpr schema::PropertyId kVolumeShadowSteps{
    .high = 0x4f52424954564f4cULL, .low = 0x5348445354455001ULL};
inline constexpr schema::PropertyId kVolumeTemporalWeight{
    .high = 0x4f52424954564f4cULL, .low = 0x54454d5057454901ULL};

// M38 authored output contract. These properties intentionally live on the
// Volume object so save/load, undo, RPC/MCP, plugins and the generic inspector
// all observe the same state as the simulation runtime.
inline constexpr schema::PropertyId kVolumeOutputParticlesEnabled{
    .high = 0x4f52424954564f4cULL, .low = 0x4f55545041525401ULL};
inline constexpr schema::PropertyId kVolumeOutputSurfaceDepositsEnabled{
    .high = 0x4f52424954564f4cULL, .low = 0x4f55545355524601ULL};
inline constexpr schema::PropertyId kVolumeOutputFieldThreshold{
    .high = 0x4f52424954564f4cULL, .low = 0x4f55545448525301ULL};
inline constexpr schema::PropertyId kVolumeOutputParticleRate{
    .high = 0x4f52424954564f4cULL, .low = 0x4f55545052544501ULL};
inline constexpr schema::PropertyId kVolumeOutputParticleBudget{
    .high = 0x4f52424954564f4cULL, .low = 0x4f55545042444701ULL};
inline constexpr schema::PropertyId kVolumeOutputSurfaceRate{
    .high = 0x4f52424954564f4cULL, .low = 0x4f55545352544501ULL};
inline constexpr schema::PropertyId kVolumeOutputSurfaceBudget{
    .high = 0x4f52424954564f4cULL, .low = 0x4f55545342444701ULL};
inline constexpr schema::PropertyId kVolumeOutputSurfaceRadius{
    .high = 0x4f52424954564f4cULL, .low = 0x4f55545352414401ULL};
inline constexpr schema::PropertyId kVolumeOutputCandidateMultiplier{
    .high = 0x4f52424954564f4cULL, .low = 0x4f555443414e4401ULL};
inline constexpr schema::PropertyId kVolumeOutputSurfaceEffect{
    .high = 0x4f52424954564f4cULL, .low = 0x4f55545345464601ULL};
inline constexpr schema::PropertyId kVolumeOutputSurfaceHalfLife{
    .high = 0x4f52424954564f4cULL, .low = 0x4f555453484c4601ULL};
inline constexpr schema::PropertyId kVolumeParticleLifetime{
    .high = 0x4f52424954564f4cULL, .low = 0x5052544c49464501ULL};
inline constexpr schema::PropertyId kVolumeParticleLinearDrag{
    .high = 0x4f52424954564f4cULL, .low = 0x5052544452414701ULL};
inline constexpr schema::PropertyId kVolumeParticleRadiusMeters{
    .high = 0x4f52424954564f4cULL, .low = 0x5052545241444901ULL};
inline constexpr schema::PropertyId kVolumeParticleEmissionScale{
    .high = 0x4f52424954564f4cULL, .low = 0x505254454d495301ULL};
inline constexpr schema::PropertyId kVolumeParticleGravityMode{
    .high = 0x4f52424954564f4cULL, .low = 0x5052544752415601ULL};
inline constexpr schema::PropertyId kVolumeParticleGravityScale{
    .high = 0x4f52424954564f4cULL, .low = 0x5052544752534301ULL};
inline constexpr schema::PropertyId kVolumeParticleCollisionMode{
    .high = 0x4f52424954564f4cULL, .low = 0x505254434f4c4c01ULL};
inline constexpr schema::PropertyId kVolumeParticleRestitution{
    .high = 0x4f52424954564f4cULL, .low = 0x5052545245535401ULL};
inline constexpr schema::PropertyId kVolumeParticleWaterDensityRatio{
    .high = 0x4f52424954564f4cULL, .low = 0x5052545744525401ULL};
inline constexpr schema::PropertyId kVolumeParticleWaterDrag{
    .high = 0x4f52424954564f4cULL, .low = 0x5052545744524701ULL};
inline constexpr schema::PropertyId kVolumeParticleWaterBuoyancyScale{
    .high = 0x4f52424954564f4cULL, .low = 0x5052545742554f01ULL};
inline constexpr schema::PropertyId kVolumeParticleKillOnWaterImmersion{
    .high = 0x4f52424954564f4cULL, .low = 0x505254574b494c01ULL};
inline constexpr schema::PropertyId kVolumeParticleSplashOnWaterEntry{
    .high = 0x4f52424954564f4cULL, .low = 0x5052545753504c01ULL};
inline constexpr schema::PropertyId kVolumeParticleWaterSplashScale{
    .high = 0x4f52424954564f4cULL, .low = 0x5052545753505301ULL};

inline constexpr schema::PropertyId kVolumeChildEnabled{
    .high = 0x4f52424954564f4cULL, .low = 0x4348454e41424c01ULL};
inline constexpr schema::PropertyId kVolumeChildKind{
    .high = 0x4f52424954564f4cULL, .low = 0x43484b494e440001ULL};
inline constexpr schema::PropertyId kVolumeChildPositionMeters{
    .high = 0x4f52424954564f4cULL, .low = 0x4348504f53495401ULL};
inline constexpr schema::PropertyId kVolumeChildRadiusMeters{
    .high = 0x4f52424954564f4cULL, .low = 0x4348524144495501ULL};
inline constexpr schema::PropertyId kVolumeChildScalar{
    .high = 0x4f52424954564f4cULL, .low = 0x43485343414c4101ULL};
inline constexpr schema::PropertyId kVolumeChildVector{
    .high = 0x4f52424954564f4cULL, .low = 0x4348564543544f01ULL};
inline constexpr schema::PropertyId kVolumeChildOrder{
    .high = 0x4f52424954564f4cULL, .low = 0x43484f5244455201ULL};
inline constexpr schema::PropertyId kVolumeChildShape{
    .high = 0x4f52424954564f4cULL, .low = 0x4348534841504501ULL};
inline constexpr schema::PropertyId kVolumeChildHalfExtentsMeters{
    .high = 0x4f52424954564f4cULL, .low = 0x434848414c464501ULL};
inline constexpr schema::PropertyId kVolumeChildFieldMask{
    .high = 0x4f52424954564f4cULL, .low = 0x43484649454c4401ULL};
inline constexpr schema::PropertyId kVolumeChildAsset{
    .high = 0x4f52424954564f4cULL, .low = 0x4348415353455401ULL};
inline constexpr schema::PropertyId kVolumeChildTargetObject{
    .high = 0x4f52424954564f4cULL, .low = 0x4348544152474501ULL};
inline constexpr schema::PropertyId kVolumeChildPaintEnabled{
    .high = 0x4f52424954564f4cULL, .low = 0x43485041494e5401ULL};

enum class VolumeSolverPolicy : i64
{
    Auto = 0,
    Surface2D5D = 1,
    Local3D = 2,
    Passive = 3
};

enum class VolumeRepresentationMode : i64
{
    Auto = 0,
    Live = 1,
    Coarse = 2,
    Passive = 3,
    Baked = 4
};

enum class VolumeSurfaceOutputEffect : i64
{
    Wetness = 0,
    Soot = 1,
    Ash = 2,
    Sediment = 3,
    Heat = 4
};

enum class VolumeParticleGravityMode : i64
{
    None = 0,
    OwningBody = 1
};

enum class VolumeParticleCollisionMode : i64
{
    None = 0,
    Kill = 1,
    Slide = 2,
    Bounce = 3
};

enum class VolumeSourceKind : i64
{
    Brush = 0,
    TextureMask = 1,
    Terrain = 2,
    Spline = 3,
    MeshSdf = 4,
    CollisionProxy = 5,
    Particles = 6,
    ObjectMotion = 7,
    WorldMotion = 8
};

enum class VolumeEffectorKind : i64
{
    Obstacle = 0,
    Drag = 1,
    Wind = 2,
    Temperature = 3,
    Dissipation = 4
};

enum class VolumeSourceShape : i64
{
    Point = 0,
    Sphere = 1,
    Box = 2,
    Spline = 3,
    Mesh = 4,
    TerrainPatch = 5
};

enum class VolumeField : u64
{
    Density = 1ULL << 0U,
    Velocity = 1ULL << 1U,
    Temperature = 1ULL << 2U,
    Pressure = 1ULL << 3U,
    Fuel = 1ULL << 4U,
    Emission = 1ULL << 5U,
    Moisture = 1ULL << 6U,
    Sediment = 1ULL << 7U
};

struct VolumeInvalidationBounds
{
    math::Double3 minimumMeters{};
    math::Double3 maximumMeters{};

    [[nodiscard]] bool IsValid() const noexcept;
};

enum class VolumeInputRole : u8
{
    Source = 0U,
    Effector = 1U
};

struct ResolvedVolumeInput
{
    scene::ObjectId object{};
    VolumeInputRole role{VolumeInputRole::Source};
    bool enabled{true};
    i64 order{0};
    i64 kind{0};
    VolumeSourceShape shape{VolumeSourceShape::Sphere};
    math::Double3 positionMeters{};
    f64 radiusMeters{1.0};
    math::Double3 halfExtentsMeters{1.0, 1.0, 1.0};
    f64 scalarValue{1.0};
    math::Double3 vectorValue{};
    u64 fieldMask{0U};
    std::string asset;
    std::optional<scene::ObjectId> targetObject;
    bool paintEnabled{false};
    VolumeInvalidationBounds bounds{};
    u64 fingerprint{0U};
};

struct ResolvedVolumeDomain
{
    scene::ObjectId object{};
    bool enabled{true};
    std::string preset{"Empty"};
    math::Double3 centerMeters{};
    math::Double3 halfExtentsMeters{10.0, 10.0, 10.0};
    VolumeSolverPolicy solverPolicy{VolumeSolverPolicy::Auto};
    VolumeRepresentationMode representationMode{
        VolumeRepresentationMode::Auto};
    u64 fieldMask{0U};
    u32 resolution{64U};
    u32 surfaceLayers{4U};

    bool renderEnabled{true};
    f32 extinctionScale{0.8F};
    f32 singleScatteringAlbedo{0.9F};
    math::Float3 scatteringColor{1.0F, 1.0F, 1.0F};
    f32 anisotropy{0.2F};
    math::Float3 emissionColor{1.0F, 0.32F, 0.06F};
    f32 emissionScale{1.0F};
    f32 giEmissionScale{1.0F};
    u32 renderSteps{64U};
    u32 shadowSteps{6U};
    f32 temporalWeight{0.85F};

    bool outputParticlesEnabled{false};
    bool outputSurfaceDepositsEnabled{false};
    f32 outputFieldThreshold{0.15F};
    f32 outputParticleRatePerSecond{64.0F};
    u32 outputParticleBudgetPerStep{256U};
    f32 outputSurfaceDepositRatePerSecond{24.0F};
    u32 outputSurfaceDepositBudgetPerStep{128U};
    f32 outputSurfaceDepositRadiusMeters{0.25F};
    u32 outputCandidateMultiplier{8U};
    VolumeSurfaceOutputEffect outputSurfaceEffect{
        VolumeSurfaceOutputEffect::Wetness};
    f32 outputSurfaceEffectHalfLifeSeconds{30.0F};

    f32 particleLifetimeSeconds{2.0F};
    f32 particleLinearDragPerSecond{0.0F};
    f32 particleRadiusMeters{0.08F};
    f32 particleEmissionScale{1.0F};
    VolumeParticleGravityMode particleGravityMode{
        VolumeParticleGravityMode::None};
    f32 particleGravityScale{1.0F};
    VolumeParticleCollisionMode particleCollisionMode{
        VolumeParticleCollisionMode::None};
    f32 particleRestitution{0.25F};
    // Relative to standing water: 1 = neutrally buoyant, <1 rises, >1 sinks.
    f32 particleWaterDensityRatio{1.0F};
    f32 particleWaterDragPerSecond{0.0F};
    f32 particleWaterBuoyancyScale{1.0F};
    bool particleKillOnWaterImmersion{false};
    bool particleSplashOnWaterEntry{false};
    f32 particleWaterSplashScale{1.0F};

    u32 sourceCount{0U};
    u32 effectorCount{0U};
};

void RegisterVolumeSchemas(
    schema::SchemaRegistry& schemas);

[[nodiscard]] std::optional<ResolvedVolumeDomain>
ResolveVolumeDomain(
    const scene::ObjectStore& objects,
    scene::ObjectId volume);

[[nodiscard]] std::string_view
VolumeSolverPolicyName(
    VolumeSolverPolicy policy) noexcept;

[[nodiscard]] std::vector<ResolvedVolumeInput>
ResolveVolumeInputs(
    const scene::ObjectStore& objects,
    scene::ObjectId volume);

[[nodiscard]] VolumeInvalidationBounds
UnionVolumeInvalidationBounds(
    const VolumeInvalidationBounds& a,
    const VolumeInvalidationBounds& b) noexcept;

[[nodiscard]] std::string_view
VolumeSourceKindName(
    VolumeSourceKind kind) noexcept;

[[nodiscard]] std::string_view
VolumeEffectorKindName(
    VolumeEffectorKind kind) noexcept;

[[nodiscard]] std::string_view
VolumeSourceShapeName(
    VolumeSourceShape shape) noexcept;

[[nodiscard]] std::string_view
VolumeRepresentationModeName(
    VolumeRepresentationMode mode) noexcept;
} // namespace orbit::world_model

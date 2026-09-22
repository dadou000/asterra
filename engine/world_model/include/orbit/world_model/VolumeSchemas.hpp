#pragma once

#include <orbit/math/Vector.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>

#include <optional>
#include <string>
#include <string_view>

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

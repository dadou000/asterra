#include <orbit/world_model/VolumeSchemas.hpp>

#include <algorithm>

namespace orbit::world_model
{
namespace
{
template <typename T>
[[nodiscard]] T Read(
    const scene::ObjectStore& objects,
    const scene::ObjectId object,
    const schema::PropertyId property,
    T fallback)
{
    const auto value =
        objects.GetProperty(
            object,
            property);

    if (!value.has_value())
    {
        return fallback;
    }

    if (const auto* typed =
            std::get_if<T>(
                &*value);
        typed != nullptr)
    {
        return *typed;
    }

    return fallback;
}
} // namespace

void RegisterVolumeSchemas(
    schema::SchemaRegistry& schemas)
{
    schemas.RegisterType({
        .id = kVolumeType,
        .displayName = "Volume",
        .category = "World / Volumetrics",
        .properties = {
            {.id=kVolumeEnabled,.name="Enabled",.kind=schema::PropertyKind::Boolean,.defaultValue=true},
            {.id=kVolumePreset,.name="Preset",.kind=schema::PropertyKind::String,.defaultValue=std::string{"Empty"}},
            {.id=kVolumeCenterMeters,.name="Center",.kind=schema::PropertyKind::Vector3,.unit="m",.defaultValue=math::Double3{}},
            {.id=kVolumeHalfExtentsMeters,.name="Half Extents",.kind=schema::PropertyKind::Vector3,.unit="m",.defaultValue=math::Double3{10.0,10.0,10.0}},
            {.id=kVolumeSolverPolicy,.name="Solver Policy",.kind=schema::PropertyKind::Integer,.defaultValue=i64{0},.range={.minimum=0.0,.maximum=3.0}},
            {.id=kVolumeRepresentationMode,.name="Representation Mode",.kind=schema::PropertyKind::Integer,.defaultValue=i64{0},.range={.minimum=0.0,.maximum=4.0}},
            {.id=kVolumeFieldMask,.name="Field Mask",.kind=schema::PropertyKind::Integer,.defaultValue=i64{0},.range={.minimum=0.0}},
            {.id=kVolumeResolution,.name="Base Resolution",.kind=schema::PropertyKind::Integer,.defaultValue=i64{64},.range={.minimum=8.0,.maximum=1024.0},.advanced=true}
        }
    });

    const auto childProperties =
        std::vector<schema::PropertySchema>{
            {.id=kVolumeChildEnabled,.name="Enabled",.kind=schema::PropertyKind::Boolean,.defaultValue=true},
            {.id=kVolumeChildKind,.name="Kind",.kind=schema::PropertyKind::Integer,.defaultValue=i64{0},.range={.minimum=0.0,.maximum=32.0}},
            {.id=kVolumeChildPositionMeters,.name="Position",.kind=schema::PropertyKind::Vector3,.unit="m",.defaultValue=math::Double3{}},
            {.id=kVolumeChildRadiusMeters,.name="Radius",.kind=schema::PropertyKind::Float,.unit="m",.defaultValue=1.0,.range={.minimum=0.0}},
            {.id=kVolumeChildScalar,.name="Scalar Value",.kind=schema::PropertyKind::Float,.defaultValue=1.0},
            {.id=kVolumeChildVector,.name="Vector Value",.kind=schema::PropertyKind::Vector3,.defaultValue=math::Double3{}}
        };

    schemas.RegisterType({
        .id = kVolumeSourceType,
        .displayName = "Volume Source",
        .category = "World / Volumetrics",
        .properties = childProperties
    });

    schemas.RegisterType({
        .id = kVolumeEffectorType,
        .displayName = "Volume Effector",
        .category = "World / Volumetrics",
        .properties = childProperties
    });
}

std::optional<ResolvedVolumeDomain>
ResolveVolumeDomain(
    const scene::ObjectStore& objects,
    const scene::ObjectId volume)
{
    const auto record =
        objects.Find(volume);

    if (!record.has_value() ||
        record->type != kVolumeType)
    {
        return std::nullopt;
    }

    ResolvedVolumeDomain result{
        .object = volume,
        .enabled = Read<bool>(objects, volume, kVolumeEnabled, true),
        .preset = Read<std::string>(objects, volume, kVolumePreset, "Empty"),
        .centerMeters = Read<math::Double3>(objects, volume, kVolumeCenterMeters, {}),
        .halfExtentsMeters = Read<math::Double3>(objects, volume, kVolumeHalfExtentsMeters, {10.0,10.0,10.0}),
        .solverPolicy = static_cast<VolumeSolverPolicy>(
            std::clamp<i64>(
                Read<i64>(objects, volume, kVolumeSolverPolicy, 0),
                0, 3)),
        .representationMode = static_cast<VolumeRepresentationMode>(
            std::clamp<i64>(
                Read<i64>(objects, volume, kVolumeRepresentationMode, 0),
                0, 4)),
        .fieldMask = static_cast<u64>(
            std::max<i64>(
                Read<i64>(objects, volume, kVolumeFieldMask, 0),
                0)),
        .resolution = static_cast<u32>(
            std::clamp<i64>(
                Read<i64>(objects, volume, kVolumeResolution, 64),
                8, 1024))
    };

    for (const auto& child :
         objects.Children(volume))
    {
        if (child.type == kVolumeSourceType)
        {
            ++result.sourceCount;
        }
        else if (child.type == kVolumeEffectorType)
        {
            ++result.effectorCount;
        }
    }

    return result;
}

std::string_view
VolumeSolverPolicyName(
    const VolumeSolverPolicy policy) noexcept
{
    switch (policy)
    {
    case VolumeSolverPolicy::Auto: return "Auto";
    case VolumeSolverPolicy::Surface2D5D: return "Surface 2D/2.5D";
    case VolumeSolverPolicy::Local3D: return "Local 3D";
    case VolumeSolverPolicy::Passive: return "Passive";
    }
    return "Unknown";
}

std::string_view
VolumeRepresentationModeName(
    const VolumeRepresentationMode mode) noexcept
{
    switch (mode)
    {
    case VolumeRepresentationMode::Auto: return "Auto";
    case VolumeRepresentationMode::Live: return "Live";
    case VolumeRepresentationMode::Coarse: return "Coarse";
    case VolumeRepresentationMode::Passive: return "Passive";
    case VolumeRepresentationMode::Baked: return "Baked";
    }
    return "Unknown";
}
} // namespace orbit::world_model

#include <orbit/world_model/VolumeSchemas.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <string>

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
            {.id=kVolumeResolution,.name="Base Resolution",.kind=schema::PropertyKind::Integer,.defaultValue=i64{64},.range={.minimum=8.0,.maximum=1024.0},.advanced=true},
            {.id=kVolumeSurfaceLayers,.name="Surface Layers",.kind=schema::PropertyKind::Integer,.defaultValue=i64{4},.range={.minimum=1.0,.maximum=32.0},.advanced=true},
            {.id=kVolumeRenderEnabled,.name="Render Enabled",.kind=schema::PropertyKind::Boolean,.defaultValue=true},
            {.id=kVolumeExtinctionScale,.name="Extinction Scale",.kind=schema::PropertyKind::Float,.defaultValue=0.8,.range={.minimum=0.0,.maximum=64.0}},
            {.id=kVolumeSingleScatteringAlbedo,.name="Single Scattering Albedo",.kind=schema::PropertyKind::Float,.defaultValue=0.9,.range={.minimum=0.0,.maximum=1.0}},
            {.id=kVolumeScatteringColor,.name="Scattering Color",.kind=schema::PropertyKind::Vector3,.defaultValue=math::Double3{1.0,1.0,1.0}},
            {.id=kVolumeAnisotropy,.name="Phase Anisotropy",.kind=schema::PropertyKind::Float,.defaultValue=0.2,.range={.minimum=-0.95,.maximum=0.95}},
            {.id=kVolumeEmissionColor,.name="Emission Color",.kind=schema::PropertyKind::Vector3,.defaultValue=math::Double3{1.0,0.32,0.06}},
            {.id=kVolumeEmissionScale,.name="Emission Scale",.kind=schema::PropertyKind::Float,.defaultValue=1.0,.range={.minimum=0.0,.maximum=1024.0}},
            {.id=kVolumeGiEmissionScale,.name="GI Emission Scale",.kind=schema::PropertyKind::Float,.defaultValue=1.0,.range={.minimum=0.0,.maximum=64.0},.advanced=true},
            {.id=kVolumeRenderSteps,.name="Raymarch Steps",.kind=schema::PropertyKind::Integer,.defaultValue=i64{64},.range={.minimum=8.0,.maximum=256.0},.advanced=true},
            {.id=kVolumeShadowSteps,.name="Shadow Steps",.kind=schema::PropertyKind::Integer,.defaultValue=i64{6},.range={.minimum=0.0,.maximum=32.0},.advanced=true},
            {.id=kVolumeTemporalWeight,.name="Temporal Weight",.kind=schema::PropertyKind::Float,.defaultValue=0.85,.range={.minimum=0.0,.maximum=0.98},.advanced=true}
        }
    });

    const auto childProperties =
        std::vector<schema::PropertySchema>{
            {.id=kVolumeChildEnabled,.name="Enabled",.kind=schema::PropertyKind::Boolean,.defaultValue=true},
            {.id=kVolumeChildKind,.name="Kind",.kind=schema::PropertyKind::Integer,.defaultValue=i64{0},.range={.minimum=0.0,.maximum=32.0}},
            {.id=kVolumeChildPositionMeters,.name="Position",.kind=schema::PropertyKind::Vector3,.unit="m",.defaultValue=math::Double3{}},
            {.id=kVolumeChildRadiusMeters,.name="Radius",.kind=schema::PropertyKind::Float,.unit="m",.defaultValue=1.0,.range={.minimum=0.0}},
            {.id=kVolumeChildScalar,.name="Scalar Value",.kind=schema::PropertyKind::Float,.defaultValue=1.0},
            {.id=kVolumeChildVector,.name="Vector Value",.kind=schema::PropertyKind::Vector3,.defaultValue=math::Double3{}},
            {.id=kVolumeChildOrder,.name="Order",.kind=schema::PropertyKind::Integer,.defaultValue=i64{0}},
            {.id=kVolumeChildShape,.name="Shape",.kind=schema::PropertyKind::Integer,.defaultValue=i64{1},.range={.minimum=0.0,.maximum=5.0}},
            {.id=kVolumeChildHalfExtentsMeters,.name="Half Extents",.kind=schema::PropertyKind::Vector3,.unit="m",.defaultValue=math::Double3{1.0,1.0,1.0}},
            {.id=kVolumeChildFieldMask,.name="Field Mask",.kind=schema::PropertyKind::Integer,.defaultValue=i64{0},.range={.minimum=0.0}},
            {.id=kVolumeChildAsset,.name="Asset / Path",.kind=schema::PropertyKind::String,.defaultValue=std::string{}},
            {.id=kVolumeChildTargetObject,.name="Target Object",.kind=schema::PropertyKind::ObjectReference,.defaultValue=schema::ObjectReferenceValue{}},
            {.id=kVolumeChildPaintEnabled,.name="Terrain Paint Enabled",.kind=schema::PropertyKind::Boolean,.defaultValue=false}
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
                8, 1024)),
        .surfaceLayers = static_cast<u32>(
            std::clamp<i64>(
                Read<i64>(objects, volume, kVolumeSurfaceLayers, 4),
                1, 32)),
        .renderEnabled =
            Read<bool>(
                objects,
                volume,
                kVolumeRenderEnabled,
                true),
        .extinctionScale =
            static_cast<f32>(
                std::max(
                    Read<f64>(
                        objects,
                        volume,
                        kVolumeExtinctionScale,
                        0.8),
                    0.0)),
        .singleScatteringAlbedo =
            static_cast<f32>(
                std::clamp(
                    Read<f64>(
                        objects,
                        volume,
                        kVolumeSingleScatteringAlbedo,
                        0.9),
                    0.0,
                    1.0)),
        .scatteringColor = [&]()
            {
                const auto value =
                    Read<math::Double3>(
                        objects,
                        volume,
                        kVolumeScatteringColor,
                        {1.0,1.0,1.0});
                return math::Float3{
                    static_cast<f32>(std::max(value.x,0.0)),
                    static_cast<f32>(std::max(value.y,0.0)),
                    static_cast<f32>(std::max(value.z,0.0))};
            }(),
        .anisotropy =
            static_cast<f32>(
                std::clamp(
                    Read<f64>(
                        objects,
                        volume,
                        kVolumeAnisotropy,
                        0.2),
                    -0.95,
                    0.95)),
        .emissionColor = [&]()
            {
                const auto value =
                    Read<math::Double3>(
                        objects,
                        volume,
                        kVolumeEmissionColor,
                        {1.0,0.32,0.06});
                return math::Float3{
                    static_cast<f32>(std::max(value.x,0.0)),
                    static_cast<f32>(std::max(value.y,0.0)),
                    static_cast<f32>(std::max(value.z,0.0))};
            }(),
        .emissionScale =
            static_cast<f32>(
                std::max(
                    Read<f64>(
                        objects,
                        volume,
                        kVolumeEmissionScale,
                        1.0),
                    0.0)),
        .giEmissionScale =
            static_cast<f32>(
                std::max(
                    Read<f64>(
                        objects,
                        volume,
                        kVolumeGiEmissionScale,
                        1.0),
                    0.0)),
        .renderSteps = static_cast<u32>(
            std::clamp<i64>(
                Read<i64>(
                    objects,
                    volume,
                    kVolumeRenderSteps,
                    64),
                8,
                256)),
        .shadowSteps = static_cast<u32>(
            std::clamp<i64>(
                Read<i64>(
                    objects,
                    volume,
                    kVolumeShadowSteps,
                    6),
                0,
                32)),
        .temporalWeight =
            static_cast<f32>(
                std::clamp(
                    Read<f64>(
                        objects,
                        volume,
                        kVolumeTemporalWeight,
                        0.85),
                    0.0,
                    0.98))
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


bool VolumeInvalidationBounds::IsValid() const noexcept
{
    return
        std::isfinite(minimumMeters.x) &&
        std::isfinite(minimumMeters.y) &&
        std::isfinite(minimumMeters.z) &&
        std::isfinite(maximumMeters.x) &&
        std::isfinite(maximumMeters.y) &&
        std::isfinite(maximumMeters.z) &&
        minimumMeters.x <= maximumMeters.x &&
        minimumMeters.y <= maximumMeters.y &&
        minimumMeters.z <= maximumMeters.z;
}

VolumeInvalidationBounds
UnionVolumeInvalidationBounds(
    const VolumeInvalidationBounds& a,
    const VolumeInvalidationBounds& b) noexcept
{
    if (!a.IsValid())
    {
        return b;
    }

    if (!b.IsValid())
    {
        return a;
    }

    return {
        .minimumMeters = {
            std::min(a.minimumMeters.x, b.minimumMeters.x),
            std::min(a.minimumMeters.y, b.minimumMeters.y),
            std::min(a.minimumMeters.z, b.minimumMeters.z)
        },
        .maximumMeters = {
            std::max(a.maximumMeters.x, b.maximumMeters.x),
            std::max(a.maximumMeters.y, b.maximumMeters.y),
            std::max(a.maximumMeters.z, b.maximumMeters.z)
        }
    };
}

namespace
{
[[nodiscard]] std::optional<scene::ObjectId>
ReadObjectReference(
    const scene::ObjectStore& objects,
    const scene::ObjectId object,
    const schema::PropertyId property)
{
    const auto value =
        objects.GetProperty(
            object,
            property);

    if (!value.has_value())
    {
        return std::nullopt;
    }

    const auto* reference =
        std::get_if<
            schema::ObjectReferenceValue>(
                &*value);

    if (reference == nullptr ||
        (reference->high == 0U &&
         reference->low == 0U))
    {
        return std::nullopt;
    }

    return scene::ObjectId{
        .high = reference->high,
        .low = reference->low
    };
}

void HashMix(
    u64& hash,
    const u64 value) noexcept
{
    hash ^= value;
    hash *=
        1099511628211ULL;
}

void HashDouble(
    u64& hash,
    const f64 value) noexcept
{
    HashMix(
        hash,
        std::bit_cast<u64>(value));
}

[[nodiscard]] u64 FingerprintInput(
    const ResolvedVolumeInput& input) noexcept
{
    u64 hash =
        1469598103934665603ULL;

    HashMix(hash, input.object.high);
    HashMix(hash, input.object.low);
    HashMix(hash, static_cast<u64>(input.role));
    HashMix(hash, input.enabled ? 1U : 0U);
    HashMix(hash, static_cast<u64>(input.order));
    HashMix(hash, static_cast<u64>(input.kind));
    HashMix(hash, static_cast<u64>(input.shape));

    HashDouble(hash, input.positionMeters.x);
    HashDouble(hash, input.positionMeters.y);
    HashDouble(hash, input.positionMeters.z);
    HashDouble(hash, input.radiusMeters);
    HashDouble(hash, input.halfExtentsMeters.x);
    HashDouble(hash, input.halfExtentsMeters.y);
    HashDouble(hash, input.halfExtentsMeters.z);
    HashDouble(hash, input.scalarValue);
    HashDouble(hash, input.vectorValue.x);
    HashDouble(hash, input.vectorValue.y);
    HashDouble(hash, input.vectorValue.z);

    HashMix(hash, input.fieldMask);
    HashMix(hash, input.paintEnabled ? 1U : 0U);

    for (const char character : input.asset)
    {
        HashMix(
            hash,
            static_cast<u64>(
                static_cast<unsigned char>(
                    character)));
    }

    if (input.targetObject.has_value())
    {
        HashMix(hash, input.targetObject->high);
        HashMix(hash, input.targetObject->low);
    }

    return hash;
}

[[nodiscard]] VolumeInvalidationBounds
BoundsForInput(
    const ResolvedVolumeInput& input) noexcept
{
    const f64 radius =
        std::max(
            input.radiusMeters,
            0.0);

    math::Double3 half =
        input.halfExtentsMeters;

    half.x =
        std::max(
            std::abs(half.x),
            radius);
    half.y =
        std::max(
            std::abs(half.y),
            radius);
    half.z =
        std::max(
            std::abs(half.z),
            radius);

    if (input.shape ==
            VolumeSourceShape::Point ||
        input.shape ==
            VolumeSourceShape::Sphere)
    {
        half = {
            radius,
            radius,
            radius
        };
    }

    if (input.shape ==
            VolumeSourceShape::Spline)
    {
        half.x +=
            std::abs(
                input.vectorValue.x);
        half.y +=
            std::abs(
                input.vectorValue.y);
        half.z +=
            std::abs(
                input.vectorValue.z);
    }

    return {
        .minimumMeters = {
            input.positionMeters.x - half.x,
            input.positionMeters.y - half.y,
            input.positionMeters.z - half.z
        },
        .maximumMeters = {
            input.positionMeters.x + half.x,
            input.positionMeters.y + half.y,
            input.positionMeters.z + half.z
        }
    };
}
} // namespace

std::vector<ResolvedVolumeInput>
ResolveVolumeInputs(
    const scene::ObjectStore& objects,
    const scene::ObjectId volume)
{
    const auto volumeRecord =
        objects.Find(volume);

    if (!volumeRecord.has_value() ||
        volumeRecord->type !=
            kVolumeType)
    {
        return {};
    }

    std::vector<ResolvedVolumeInput>
        result;

    for (const auto& child :
         objects.Children(volume))
    {
        VolumeInputRole role{};

        if (child.type ==
            kVolumeSourceType)
        {
            role =
                VolumeInputRole::Source;
        }
        else if (child.type ==
                 kVolumeEffectorType)
        {
            role =
                VolumeInputRole::Effector;
        }
        else
        {
            continue;
        }

        ResolvedVolumeInput input{
            .object = child.id,
            .role = role,
            .enabled =
                Read<bool>(
                    objects,
                    child.id,
                    kVolumeChildEnabled,
                    true),
            .order =
                Read<i64>(
                    objects,
                    child.id,
                    kVolumeChildOrder,
                    child.sortOrder),
            .kind =
                Read<i64>(
                    objects,
                    child.id,
                    kVolumeChildKind,
                    0),
            .shape =
                static_cast<VolumeSourceShape>(
                    std::clamp<i64>(
                        Read<i64>(
                            objects,
                            child.id,
                            kVolumeChildShape,
                            1),
                        0,
                        5)),
            .positionMeters =
                Read<math::Double3>(
                    objects,
                    child.id,
                    kVolumeChildPositionMeters,
                    {}),
            .radiusMeters =
                std::max(
                    Read<f64>(
                        objects,
                        child.id,
                        kVolumeChildRadiusMeters,
                        1.0),
                    0.0),
            .halfExtentsMeters =
                Read<math::Double3>(
                    objects,
                    child.id,
                    kVolumeChildHalfExtentsMeters,
                    {1.0, 1.0, 1.0}),
            .scalarValue =
                Read<f64>(
                    objects,
                    child.id,
                    kVolumeChildScalar,
                    1.0),
            .vectorValue =
                Read<math::Double3>(
                    objects,
                    child.id,
                    kVolumeChildVector,
                    {}),
            .fieldMask =
                static_cast<u64>(
                    std::max<i64>(
                        Read<i64>(
                            objects,
                            child.id,
                            kVolumeChildFieldMask,
                            0),
                        0)),
            .asset =
                Read<std::string>(
                    objects,
                    child.id,
                    kVolumeChildAsset,
                    {}),
            .targetObject =
                ReadObjectReference(
                    objects,
                    child.id,
                    kVolumeChildTargetObject),
            .paintEnabled =
                Read<bool>(
                    objects,
                    child.id,
                    kVolumeChildPaintEnabled,
                    false)
        };

        input.bounds =
            BoundsForInput(input);
        input.fingerprint =
            FingerprintInput(input);

        result.push_back(
            std::move(input));
    }

    std::stable_sort(
        result.begin(),
        result.end(),
        [](
            const ResolvedVolumeInput& a,
            const ResolvedVolumeInput& b)
        {
            if (a.order != b.order)
            {
                return a.order < b.order;
            }

            return
                a.object.ToString() <
                b.object.ToString();
        });

    return result;
}

std::string_view
VolumeSourceKindName(
    const VolumeSourceKind kind) noexcept
{
    switch (kind)
    {
    case VolumeSourceKind::Brush: return "Brush";
    case VolumeSourceKind::TextureMask: return "Texture / Mask";
    case VolumeSourceKind::Terrain: return "Terrain Paint";
    case VolumeSourceKind::Spline: return "Spline";
    case VolumeSourceKind::MeshSdf: return "Mesh / SDF";
    case VolumeSourceKind::CollisionProxy: return "Collision Proxy";
    case VolumeSourceKind::Particles: return "Particles";
    case VolumeSourceKind::ObjectMotion: return "Object Motion";
    case VolumeSourceKind::WorldMotion: return "World Motion";
    }

    return "Unknown";
}

std::string_view
VolumeEffectorKindName(
    const VolumeEffectorKind kind) noexcept
{
    switch (kind)
    {
    case VolumeEffectorKind::Obstacle: return "Obstacle";
    case VolumeEffectorKind::Drag: return "Drag";
    case VolumeEffectorKind::Wind: return "Wind";
    case VolumeEffectorKind::Temperature: return "Temperature";
    case VolumeEffectorKind::Dissipation: return "Dissipation";
    }

    return "Unknown";
}

std::string_view
VolumeSourceShapeName(
    const VolumeSourceShape shape) noexcept
{
    switch (shape)
    {
    case VolumeSourceShape::Point: return "Point";
    case VolumeSourceShape::Sphere: return "Sphere";
    case VolumeSourceShape::Box: return "Box";
    case VolumeSourceShape::Spline: return "Spline";
    case VolumeSourceShape::Mesh: return "Mesh";
    case VolumeSourceShape::TerrainPatch: return "Terrain Patch";
    }

    return "Unknown";
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

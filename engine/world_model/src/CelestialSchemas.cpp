#include <orbit/world_model/CelestialSchemas.hpp>

#include <string>
#include <utility>

namespace orbit::world_model
{
namespace
{
schema::PropertySchema EnabledProperty()
{
    return {
        .id = kCapabilityEnabled,
        .name = "Enabled",
        .kind = schema::PropertyKind::Boolean,
        .defaultValue = true
    };
}

schema::PropertySchema ModelProperty(std::string defaultModel)
{
    return {
        .id = kCapabilityModel,
        .name = "Model",
        .kind = schema::PropertyKind::String,
        .defaultValue = std::move(defaultModel),
        .advanced = true
    };
}

schema::PropertySchema SourceObjectProperty()
{
    return {
        .id = kCapabilitySourceObject,
        .name = "Source Object",
        .kind = schema::PropertyKind::ObjectReference,
        .defaultValue = schema::ObjectReferenceValue{},
        .advanced = true
    };
}

void RegisterCapability(
    schema::SchemaRegistry& schemas,
    const schema::TypeId id,
    std::string displayName,
    std::string defaultModel,
    const bool sourceObject = false)
{
    schema::TypeSchema type{
        .id = id,
        .displayName = std::move(displayName),
        .category = "Celestial/Capability",
        .properties = {
            EnabledProperty(),
            ModelProperty(std::move(defaultModel))
        }
    };

    if (sourceObject)
    {
        type.properties.push_back(SourceObjectProperty());
    }

    schemas.RegisterType(std::move(type));
}
} // namespace

void RegisterCelestialCapabilitySchemas(
    schema::SchemaRegistry& schemas)
{
    RegisterCapability(
        schemas, kReferenceShapeCapabilityType,
        "Reference Shape", "Sphere");
    RegisterCapability(
        schemas, kMassPropertiesCapabilityType,
        "Mass Properties", "Explicit");
    RegisterCapability(
        schemas, kOrbitCapabilityType,
        "Orbit / Ephemeris", "Fixed", true);
    RegisterCapability(
        schemas, kRotationCapabilityType,
        "Rotation / Orientation", "Uniform Spin", true);
    RegisterCapability(
        schemas, kSurfaceCapabilityType,
        "Surface", "Terrain Authority", true);
    RegisterCapability(
        schemas, kAtmosphereCapabilityType,
        "Atmosphere", "Physical");
    RegisterCapability(
        schemas, kOceanCapabilityType,
        "Ocean", "Surface Authority", true);
    RegisterCapability(
        schemas, kCloudLayerCapabilityType,
        "Cloud Layer", "Procedural");
    RegisterCapability(
        schemas, kRingSystemCapabilityType,
        "Ring System", "Particle Distribution");
    RegisterCapability(
        schemas, kRingBandType,
        "Ring Band", "Physical Band");
    RegisterCapability(
        schemas, kRadiativeEmitterCapabilityType,
        "Radiative Emitter", "Blackbody");
    RegisterCapability(
        schemas, kPhotosphereCapabilityType,
        "Photosphere", "Procedural");
    RegisterCapability(
        schemas, kMagnetosphereCapabilityType,
        "Magnetosphere / Aurora", "Parameterized");
    RegisterCapability(
        schemas, kCometTailCapabilityType,
        "Comet Tail", "Solar Driven");
    RegisterCapability(
        schemas, kCompactObjectCapabilityType,
        "Compact Object", "Disabled");
    RegisterCapability(
        schemas, kAccretionFlowCapabilityType,
        "Accretion Flow", "Disabled");
}
} // namespace orbit::world_model

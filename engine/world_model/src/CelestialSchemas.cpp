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
    schemas.RegisterType({
        .id = kEphemerisAssetType,
        .displayName = "Ephemeris Asset",
        .category = "Celestial/Imported Data",
        .properties = {
            {.id = kEphemerisSourceLabel,
             .name = "Source Label",
             .kind = schema::PropertyKind::String,
             .defaultValue = std::string{}}
        }
    });

    schemas.RegisterType({
        .id = kEphemerisSampleType,
        .displayName = "Ephemeris Sample",
        .category = "Celestial/Imported Data",
        .properties = {
            {.id = kEphemerisSampleTimeMicroseconds,
             .name = "Time",
             .kind = schema::PropertyKind::Integer,
             .unit = "us",
             .defaultValue = i64{0}},
            {.id = kEphemerisSamplePositionMeters,
             .name = "Position",
             .kind = schema::PropertyKind::Vector3,
             .unit = "m",
             .defaultValue = math::Double3{}},
            {.id = kEphemerisSampleVelocityMetersPerSecond,
             .name = "Velocity",
             .kind = schema::PropertyKind::Vector3,
             .unit = "m/s",
             .defaultValue = math::Double3{}}
        }
    });

    RegisterCapability(
        schemas, kReferenceShapeCapabilityType,
        "Reference Shape", "Sphere");
    RegisterCapability(
        schemas, kMassPropertiesCapabilityType,
        "Mass Properties", "Explicit");
    schemas.RegisterType({
        .id = kOrbitCapabilityType,
        .displayName = "Orbit / Ephemeris",
        .category = "Celestial/Capability",
        .properties = {
            EnabledProperty(),
            ModelProperty("Fixed"),
            SourceObjectProperty(),
            {.id = kOrbitSemiMajorAxisMeters,
             .name = "Semi-major Axis Magnitude",
             .kind = schema::PropertyKind::Float,
             .unit = "m",
             .defaultValue = 1.0,
             .range = {.minimum = 0.000001}},
            {.id = kOrbitPeriapsisDistanceMeters,
             .name = "Periapsis Distance",
             .kind = schema::PropertyKind::Float,
             .unit = "m",
             .defaultValue = 1.0,
             .range = {.minimum = 0.000001}},
            {.id = kOrbitEccentricity,
             .name = "Eccentricity",
             .kind = schema::PropertyKind::Float,
             .defaultValue = 0.0,
             .range = {.minimum = 0.0}},
            {.id = kOrbitInclinationDegrees,
             .name = "Inclination",
             .kind = schema::PropertyKind::Float,
             .unit = "deg",
             .defaultValue = 0.0},
            {.id = kOrbitAscendingNodeDegrees,
             .name = "Longitude of Ascending Node",
             .kind = schema::PropertyKind::Float,
             .unit = "deg",
             .defaultValue = 0.0},
            {.id = kOrbitArgumentPeriapsisDegrees,
             .name = "Argument of Periapsis",
             .kind = schema::PropertyKind::Float,
             .unit = "deg",
             .defaultValue = 0.0},
            {.id = kOrbitMeanAnomalyEpochDegrees,
             .name = "Mean Anomaly At Epoch",
             .kind = schema::PropertyKind::Float,
             .unit = "deg",
             .defaultValue = 0.0},
            {.id = kOrbitBarkerParameterEpoch,
             .name = "Barker Parameter At Epoch",
             .kind = schema::PropertyKind::Float,
             .defaultValue = 0.0,
             .advanced = true},
            {.id = kOrbitGravitationalParameter,
             .name = "Gravitational Parameter",
             .kind = schema::PropertyKind::Float,
             .unit = "m3/s2",
             .defaultValue = 1.0,
             .range = {.minimum = 0.000001}},
            {.id = kOrbitEpochMicroseconds,
             .name = "Orbit Epoch",
             .kind = schema::PropertyKind::Integer,
             .unit = "us",
             .defaultValue = i64{0},
             .advanced = true},
            {.id = kOrbitDynamicPromotionEnabled,
             .name = "Dynamic N-Body Promotion",
             .kind = schema::PropertyKind::Boolean,
             .defaultValue = false,
             .advanced = true},
            {.id = kOrbitDynamicStepSeconds,
             .name = "N-Body Step",
             .kind = schema::PropertyKind::Float,
             .unit = "s",
             .defaultValue = 60.0,
             .range = {.minimum = 0.000001},
             .advanced = true},
            {.id = kOrbitDynamicSofteningMeters,
             .name = "N-Body Softening",
             .kind = schema::PropertyKind::Float,
             .unit = "m",
             .defaultValue = 0.0,
             .range = {.minimum = 0.0},
             .advanced = true}
        }
    });
    schemas.RegisterType({
        .id = kGravityCapabilityType,
        .displayName = "Gravity",
        .category = "Celestial/Capability",
        .properties = {
            EnabledProperty(),
            ModelProperty("Point Mass"),
            {.id = kGravityDeriveMuFromMass,
             .name = "Derive Mu From Body Mass",
             .kind = schema::PropertyKind::Boolean,
             .defaultValue = true},
            {.id = kGravityMuM3PerS2,
             .name = "Gravitational Parameter",
             .kind = schema::PropertyKind::Float,
             .unit = "m3/s2",
             .defaultValue = 1.0,
             .range = {.minimum = 0.0}},
            {.id = kGravitySofteningMeters,
             .name = "Softening",
             .kind = schema::PropertyKind::Float,
             .unit = "m",
             .defaultValue = 0.0,
             .range = {.minimum = 0.0},
             .advanced = true}
        }
    });

    schemas.RegisterType({
        .id = kRotationCapabilityType,
        .displayName = "Rotation / Orientation",
        .category = "Celestial/Capability",
        .properties = {
            EnabledProperty(),
            ModelProperty("Uniform Spin"),
            SourceObjectProperty(),
            {.id = kRotationAxis,
             .name = "Pole Axis",
             .kind = schema::PropertyKind::Vector3,
             .defaultValue = math::Double3{0.0, 0.0, 1.0}},
            {.id = kRotationPeriodSeconds,
             .name = "Rotation Period",
             .kind = schema::PropertyKind::Float,
             .unit = "s",
             .defaultValue = 86'400.0,
             .range = {.minimum = 0.000001}},
            {.id = kRotationPhaseDegrees,
             .name = "Phase At Epoch",
             .kind = schema::PropertyKind::Float,
             .unit = "deg",
             .defaultValue = 0.0},
            {.id = kRotationEpochMicroseconds,
             .name = "Rotation Epoch",
             .kind = schema::PropertyKind::Integer,
             .unit = "us",
             .defaultValue = i64{0},
             .advanced = true},
            {.id = kRotationSynchronousPhaseOffsetDegrees,
             .name = "Synchronous Phase Offset",
             .kind = schema::PropertyKind::Float,
             .unit = "deg",
             .defaultValue = 0.0,
             .advanced = true}
        }
    });
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
    schemas.RegisterType({
        .id = kRadiativeEmitterCapabilityType,
        .displayName = "Radiative Emitter",
        .category = "Celestial/Capability",
        .properties = {
            EnabledProperty(),
            ModelProperty("Blackbody"),
            {.id = kEmitterLuminosityWatts,
             .name = "Bolometric Luminosity",
             .kind = schema::PropertyKind::Float,
             .unit = "W",
             .defaultValue = 3.828e26,
             .range = {.minimum = 0.0}},
            {.id = kEmitterEffectiveTemperatureKelvin,
             .name = "Effective Temperature",
             .kind = schema::PropertyKind::Float,
             .unit = "K",
             .defaultValue = 5772.0,
             .range = {.minimum = 0.000001}},
            {.id = kEmitterEmissivity,
             .name = "Emissivity",
             .kind = schema::PropertyKind::Float,
             .defaultValue = 1.0,
             .range = {.minimum = 0.0, .maximum = 1.0}},
            {.id = kEmitterDeriveLuminosity,
             .name = "Derive Luminosity From Photosphere",
             .kind = schema::PropertyKind::Boolean,
             .defaultValue = true}
        }
    });

    schemas.RegisterType({
        .id = kPhotosphereCapabilityType,
        .displayName = "Photosphere",
        .category = "Celestial/Capability",
        .properties = {
            EnabledProperty(),
            ModelProperty("Blackbody"),
            {.id = kPhotosphereRadiusMeters,
             .name = "Photosphere Radius",
             .kind = schema::PropertyKind::Float,
             .unit = "m",
             .defaultValue = 6.957e8,
             .range = {.minimum = 0.000001}},
            {.id = kPhotosphereTemperatureKelvin,
             .name = "Effective Temperature",
             .kind = schema::PropertyKind::Float,
             .unit = "K",
             .defaultValue = 5772.0,
             .range = {.minimum = 0.000001}}
        }
    });
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

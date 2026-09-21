#include <orbit/editor_model/CelestialRecipeService.hpp>

#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <stdexcept>

namespace orbit::editor_model
{
namespace
{
constexpr f64 kG = 6.67430e-11;
constexpr f64 kEarthRadius = 6.371e6;
constexpr f64 kAstronomicalUnit = 1.495978707e11;

class SplitMix64
{
public:
    explicit SplitMix64(const u64 seed)
        : state_(seed)
    {
    }

    [[nodiscard]] u64 NextU64() noexcept
    {
        u64 z =
            (state_ +=
                0x9E3779B97F4A7C15ULL);
        z =
            (z ^ (z >> 30U)) *
            0xBF58476D1CE4E5B9ULL;
        z =
            (z ^ (z >> 27U)) *
            0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }

    [[nodiscard]] f64 Unit() noexcept
    {
        return static_cast<f64>(
            NextU64() >> 11U) *
            (1.0 / 9007199254740992.0);
    }

    [[nodiscard]] f64 Range(
        const f64 minimum,
        const f64 maximum) noexcept
    {
        return minimum +
            (maximum - minimum) * Unit();
    }

    [[nodiscard]] bool Chance(
        const f64 probability) noexcept
    {
        return Unit() < probability;
    }

private:
    u64 state_{0};
};

[[nodiscard]] f64 SphereMass(
    const f64 radiusMeters,
    const f64 densityKilogramsPerCubicMeter)
{
    if (!std::isfinite(radiusMeters) ||
        radiusMeters <= 0.0 ||
        !std::isfinite(
            densityKilogramsPerCubicMeter) ||
        densityKilogramsPerCubicMeter <= 0.0)
    {
        throw std::invalid_argument(
            "Recipe radius and density must be finite and positive.");
    }

    return
        (4.0 / 3.0) *
        std::numbers::pi_v<f64> *
        radiusMeters *
        radiusMeters *
        radiusMeters *
        densityKilogramsPerCubicMeter;
}

void ValidateParent(
    const scene::ObjectStore& objects,
    const scene::ObjectId parent,
    const schema::TypeId expected,
    const char* message)
{
    const auto record =
        objects.Find(parent);

    if (!record.has_value() ||
        record->type != expected)
    {
        throw std::invalid_argument(message);
    }
}

void SetCommonBody(
    commands::CommandService& commands,
    const scene::ObjectId body,
    const f64 radiusMeters,
    const f64 massKilograms,
    const f64 rotationPeriodSeconds,
    const f64 axialTiltDegrees)
{
    commands.SetProperty(
        body,
        world_model::kBodyRadius,
        radiusMeters);
    commands.SetProperty(
        body,
        world_model::kBodyMass,
        massKilograms);
    commands.SetProperty(
        body,
        world_model::kBodyRotationPeriodSeconds,
        rotationPeriodSeconds);
    commands.SetProperty(
        body,
        world_model::kBodyAxialTiltDegrees,
        axialTiltDegrees);
}

[[nodiscard]] scene::ObjectId AddCapability(
    commands::CommandService& commands,
    const scene::ObjectId body,
    const schema::TypeId type,
    const std::string_view name,
    const std::string_view model)
{
    const auto capability =
        commands.CreateObject(
            type,
            name,
            body);

    commands.SetProperty(
        capability,
        world_model::kCapabilityModel,
        std::string(model));

    return capability;
}

void ConfigureAnalyticOrbit(
    commands::CommandService& commands,
    const scene::ObjectId body,
    const f64 semiMajorAxisMeters,
    const f64 eccentricity,
    const f64 inclinationDegrees,
    const f64 ascendingNodeDegrees,
    const f64 argumentPeriapsisDegrees,
    const f64 meanAnomalyDegrees,
    const f64 mu)
{
    if (!std::isfinite(semiMajorAxisMeters) ||
        semiMajorAxisMeters <= 0.0 ||
        !std::isfinite(eccentricity) ||
        eccentricity < 0.0 ||
        !std::isfinite(mu) ||
        mu <= 0.0)
    {
        throw std::invalid_argument(
            "Recipe analytic orbit parameters are invalid.");
    }

    const auto orbit =
        AddCapability(
            commands,
            body,
            world_model::kOrbitCapabilityType,
            "Orbit",
            "Analytic Conic");

    commands.SetProperty(
        orbit,
        world_model::kOrbitSemiMajorAxisMeters,
        semiMajorAxisMeters);
    commands.SetProperty(
        orbit,
        world_model::kOrbitPeriapsisDistanceMeters,
        std::max(
            semiMajorAxisMeters *
                (1.0 - eccentricity),
            1.0e-6));
    commands.SetProperty(
        orbit,
        world_model::kOrbitEccentricity,
        eccentricity);
    commands.SetProperty(
        orbit,
        world_model::kOrbitInclinationDegrees,
        inclinationDegrees);
    commands.SetProperty(
        orbit,
        world_model::kOrbitAscendingNodeDegrees,
        ascendingNodeDegrees);
    commands.SetProperty(
        orbit,
        world_model::kOrbitArgumentPeriapsisDegrees,
        argumentPeriapsisDegrees);
    commands.SetProperty(
        orbit,
        world_model::kOrbitMeanAnomalyEpochDegrees,
        meanAnomalyDegrees);
    commands.SetProperty(
        orbit,
        world_model::kOrbitGravitationalParameter,
        mu);
}

void ConfigureGravity(
    commands::CommandService& commands,
    const scene::ObjectId body)
{
    const auto gravity =
        AddCapability(
            commands,
            body,
            world_model::kGravityCapabilityType,
            "Gravity",
            "Point Mass");

    commands.SetProperty(
        gravity,
        world_model::kGravityDeriveMuFromMass,
        true);
}

void ConfigureUniformRotation(
    commands::CommandService& commands,
    const scene::ObjectId body,
    const f64 periodSeconds,
    const f64 axialTiltDegrees,
    const f64 phaseDegrees)
{
    const auto rotation =
        AddCapability(
            commands,
            body,
            world_model::kRotationCapabilityType,
            "Rotation",
            "Uniform Spin");

    const f64 tilt =
        axialTiltDegrees *
        std::numbers::pi_v<f64> /
        180.0;

    commands.SetProperty(
        rotation,
        world_model::kRotationAxis,
        math::Double3{
            0.0,
            std::sin(tilt),
            std::cos(tilt)
        });
    commands.SetProperty(
        rotation,
        world_model::kRotationPeriodSeconds,
        periodSeconds);
    commands.SetProperty(
        rotation,
        world_model::kRotationPhaseDegrees,
        phaseDegrees);
}
} // namespace

CelestialRecipeService::CelestialRecipeService(
    scene::ObjectStore& objects,
    commands::CommandService& commands)
    : objects_(objects),
      commands_(commands)
{
}

scene::ObjectId
CelestialRecipeService::CreateStarInternal(
    const scene::ObjectId parent,
    const StarRecipe& recipe)
{
    if (!std::isfinite(recipe.massKilograms) ||
        recipe.massKilograms <= 0.0 ||
        !std::isfinite(recipe.radiusMeters) ||
        recipe.radiusMeters <= 0.0 ||
        !std::isfinite(
            recipe.effectiveTemperatureKelvin) ||
        recipe.effectiveTemperatureKelvin <= 0.0 ||
        !std::isfinite(
            recipe.rotationPeriodSeconds) ||
        recipe.rotationPeriodSeconds <= 0.0)
    {
        throw std::invalid_argument(
            "Star recipe physical parameters are invalid.");
    }

    SplitMix64 random(recipe.seed);

    const auto body =
        commands_.CreateObject(
            world_model::kCelestialBodyType,
            recipe.name,
            parent);

    SetCommonBody(
        commands_,
        body,
        recipe.radiusMeters,
        recipe.massKilograms,
        recipe.rotationPeriodSeconds,
        random.Range(0.0, 12.0));

    ConfigureGravity(
        commands_,
        body);

    ConfigureUniformRotation(
        commands_,
        body,
        recipe.rotationPeriodSeconds,
        random.Range(0.0, 12.0),
        random.Range(0.0, 360.0));

    const auto emitter =
        AddCapability(
            commands_,
            body,
            world_model::kRadiativeEmitterCapabilityType,
            "Radiative Emitter",
            "Blackbody");

    commands_.SetProperty(
        emitter,
        world_model::kEmitterEffectiveTemperatureKelvin,
        recipe.effectiveTemperatureKelvin);
    commands_.SetProperty(
        emitter,
        world_model::kEmitterEmissivity,
        1.0);
    commands_.SetProperty(
        emitter,
        world_model::kEmitterDeriveLuminosity,
        true);

    const auto photosphere =
        AddCapability(
            commands_,
            body,
            world_model::kPhotosphereCapabilityType,
            "Photosphere",
            "Blackbody");

    commands_.SetProperty(
        photosphere,
        world_model::kPhotosphereRadiusMeters,
        recipe.radiusMeters);
    commands_.SetProperty(
        photosphere,
        world_model::kPhotosphereTemperatureKelvin,
        recipe.effectiveTemperatureKelvin);

    return body;
}

scene::ObjectId
CelestialRecipeService::CreateRockyPlanetInternal(
    const scene::ObjectId parent,
    const RockyPlanetRecipe& recipe)
{
    SplitMix64 random(recipe.seed);

    const f64 mass =
        SphereMass(
            recipe.radiusMeters,
            recipe.
                densityKilogramsPerCubicMeter);

    const f64 rotationPeriod =
        random.Range(
            14.0 * 3600.0,
            40.0 * 3600.0);
    const f64 tilt =
        random.Range(0.0, 35.0);

    const auto body =
        commands_.CreateObject(
            world_model::kCelestialBodyType,
            recipe.name,
            parent);

    SetCommonBody(
        commands_,
        body,
        recipe.radiusMeters,
        mass,
        rotationPeriod,
        tilt);

    ConfigureGravity(
        commands_,
        body);

    ConfigureUniformRotation(
        commands_,
        body,
        rotationPeriod,
        tilt,
        random.Range(0.0, 360.0));

    ConfigureAnalyticOrbit(
        commands_,
        body,
        recipe.semiMajorAxisMeters,
        recipe.eccentricity,
        recipe.inclinationDegrees,
        random.Range(0.0, 360.0),
        random.Range(0.0, 360.0),
        random.Range(0.0, 360.0),
        recipe.centralMuM3PerS2);

    static_cast<void>(
        AddCapability(
            commands_,
            body,
            world_model::kSurfaceCapabilityType,
            "Surface",
            "Terrain Authority"));

    if (recipe.atmosphere)
    {
        const auto atmosphere =
            AddCapability(
                commands_,
                body,
                world_model::kAtmosphereCapabilityType,
                "Atmosphere",
                "Physical Scattering");

        commands_.SetProperty(
            atmosphere,
            world_model::kAtmosphereTopRadiusMeters,
            recipe.radiusMeters +
                std::max(
                    80'000.0,
                    recipe.radiusMeters *
                        0.012));
    }

    if (recipe.ocean)
    {
        static_cast<void>(
            AddCapability(
                commands_,
                body,
                world_model::kOceanCapabilityType,
                "Ocean",
                "Surface Authority"));
    }

    return body;
}

scene::ObjectId
CelestialRecipeService::CreateMoonInternal(
    const scene::ObjectId parentBody,
    const MoonRecipe& recipe)
{
    SplitMix64 random(recipe.seed);

    const f64 mass =
        SphereMass(
            recipe.radiusMeters,
            recipe.
                densityKilogramsPerCubicMeter);

    const auto body =
        commands_.CreateObject(
            world_model::kCelestialBodyType,
            recipe.name,
            parentBody);

    const f64 orbitalPeriod =
        2.0 *
        std::numbers::pi_v<f64> *
        std::sqrt(
            recipe.semiMajorAxisMeters *
            recipe.semiMajorAxisMeters *
            recipe.semiMajorAxisMeters /
            recipe.centralMuM3PerS2);

    SetCommonBody(
        commands_,
        body,
        recipe.radiusMeters,
        mass,
        orbitalPeriod,
        recipe.inclinationDegrees);

    ConfigureGravity(
        commands_,
        body);

    ConfigureAnalyticOrbit(
        commands_,
        body,
        recipe.semiMajorAxisMeters,
        recipe.eccentricity,
        recipe.inclinationDegrees,
        random.Range(0.0, 360.0),
        random.Range(0.0, 360.0),
        random.Range(0.0, 360.0),
        recipe.centralMuM3PerS2);

    const auto rotation =
        AddCapability(
            commands_,
            body,
            world_model::kRotationCapabilityType,
            "Rotation",
            recipe.synchronousRotation
                ? "Synchronous"
                : "Uniform Spin");

    commands_.SetProperty(
        rotation,
        world_model::kRotationAxis,
        math::Double3{
            0.0,
            0.0,
            1.0
        });

    if (!recipe.synchronousRotation)
    {
        commands_.SetProperty(
            rotation,
            world_model::kRotationPeriodSeconds,
            orbitalPeriod);
    }

    static_cast<void>(
        AddCapability(
            commands_,
            body,
            world_model::kSurfaceCapabilityType,
            "Surface",
            "Terrain Authority"));

    return body;
}

scene::ObjectId CelestialRecipeService::CreateStar(
    const scene::ObjectId parent,
    const StarRecipe& recipe)
{
    const auto parentRecord =
        objects_.Find(parent);

    if (!parentRecord.has_value() ||
        (parentRecord->type !=
             world_model::kCelestialSystemType &&
         parentRecord->type !=
             world_model::kCelestialReferenceNodeType))
    {
        throw std::invalid_argument(
            "Star recipe parent must be a celestial system or reference node.");
    }

    commands_.BeginTransaction(
        "Create Star Recipe");

    try
    {
        const auto body =
            CreateStarInternal(
                parent,
                recipe);
        commands_.CommitTransaction();
        return body;
    }
    catch (...)
    {
        if (commands_.HasActiveTransaction())
        {
            commands_.RollbackTransaction();
        }
        throw;
    }
}

scene::ObjectId
CelestialRecipeService::CreateRockyPlanet(
    const scene::ObjectId parent,
    const RockyPlanetRecipe& recipe)
{
    const auto parentRecord =
        objects_.Find(parent);

    if (!parentRecord.has_value() ||
        (parentRecord->type !=
             world_model::kCelestialSystemType &&
         parentRecord->type !=
             world_model::kCelestialReferenceNodeType))
    {
        throw std::invalid_argument(
            "Rocky-planet recipe parent must be a celestial system or reference node.");
    }

    commands_.BeginTransaction(
        "Create Rocky Planet Recipe");

    try
    {
        const auto body =
            CreateRockyPlanetInternal(
                parent,
                recipe);
        commands_.CommitTransaction();
        return body;
    }
    catch (...)
    {
        if (commands_.HasActiveTransaction())
        {
            commands_.RollbackTransaction();
        }
        throw;
    }
}

scene::ObjectId CelestialRecipeService::CreateMoon(
    const scene::ObjectId parentBody,
    const MoonRecipe& recipe)
{
    ValidateParent(
        objects_,
        parentBody,
        world_model::kCelestialBodyType,
        "Moon recipe parent must be a celestial body.");

    commands_.BeginTransaction(
        "Create Moon Recipe");

    try
    {
        const auto body =
            CreateMoonInternal(
                parentBody,
                recipe);
        commands_.CommitTransaction();
        return body;
    }
    catch (...)
    {
        if (commands_.HasActiveTransaction())
        {
            commands_.RollbackTransaction();
        }
        throw;
    }
}

SeededSystemResult
CelestialRecipeService::CreateSeededSystem(
    const scene::ObjectId world,
    const SeededSystemRecipe& recipe)
{
    ValidateParent(
        objects_,
        world,
        world_model::kWorldType,
        "Seeded system recipe parent must be World.");

    if (recipe.rockyPlanetCount == 0U)
    {
        throw std::invalid_argument(
            "Seeded system recipe requires at least one planet.");
    }

    commands_.BeginTransaction(
        "Generate Celestial System Recipe");

    try
    {
        SplitMix64 random(recipe.seed);

        SeededSystemResult result;

        result.system =
            commands_.CreateObject(
                world_model::kCelestialSystemType,
                recipe.systemName,
                world);

        commands_.SetProperty(
            result.system,
            world_model::kSystemEpochMicroseconds,
            i64{0});

        const f64 starMassScale =
            random.Range(0.82, 1.18);
        const f64 starRadiusScale =
            std::pow(
                starMassScale,
                0.80);

        StarRecipe star{
            .seed = random.NextU64(),
            .name = recipe.starName,
            .massKilograms =
                1.98847e30 *
                starMassScale,
            .radiusMeters =
                6.957e8 *
                starRadiusScale,
            .effectiveTemperatureKelvin =
                5772.0 *
                std::pow(
                    starMassScale,
                    0.50),
            .rotationPeriodSeconds =
                random.Range(
                    12.0,
                    34.0) *
                86'400.0
        };

        result.star =
            CreateStarInternal(
                result.system,
                star);

        const f64 starMu =
            kG * star.massKilograms;

        f64 previousAxis =
            0.32 * kAstronomicalUnit;

        result.planets.reserve(
            recipe.rockyPlanetCount);

        for (u32 index = 0;
             index <
                 recipe.rockyPlanetCount;
             ++index)
        {
            const f64 spacing =
                random.Range(1.55, 2.05);

            const f64 axis =
                previousAxis *
                (index == 0U
                     ? random.Range(1.0, 1.35)
                     : spacing);

            previousAxis = axis;

            const f64 radiusScale =
                random.Range(0.58, 1.42);

            const f64 density =
                random.Range(
                    3900.0,
                    6900.0);

            RockyPlanetRecipe planet{
                .seed = random.NextU64(),
                .name =
                    std::format(
                        "Planet {}",
                        index + 1U),
                .radiusMeters =
                    kEarthRadius *
                    radiusScale,
                .densityKilogramsPerCubicMeter =
                    density,
                .semiMajorAxisMeters = axis,
                .eccentricity =
                    random.Range(0.0, 0.11),
                .inclinationDegrees =
                    random.Range(0.0, 4.0),
                .centralMuM3PerS2 =
                    starMu,
                .atmosphere =
                    random.Chance(0.78),
                .ocean =
                    random.Chance(0.52)
            };

            const auto planetId =
                CreateRockyPlanetInternal(
                    result.system,
                    planet);

            result.planets.push_back(
                planetId);

            if (recipe.generateMoons &&
                random.Chance(0.58))
            {
                const f64 planetMass =
                    SphereMass(
                        planet.radiusMeters,
                        planet.
                            densityKilogramsPerCubicMeter);

                const f64 planetMu =
                    kG * planetMass;

                MoonRecipe moon{
                    .seed = random.NextU64(),
                    .name =
                        std::format(
                            "Planet {} Moon",
                            index + 1U),
                    .radiusMeters =
                        planet.radiusMeters *
                        random.Range(
                            0.12,
                            0.34),
                    .densityKilogramsPerCubicMeter =
                        random.Range(
                            2300.0,
                            4100.0),
                    .semiMajorAxisMeters =
                        planet.radiusMeters *
                        random.Range(
                            18.0,
                            55.0),
                    .eccentricity =
                        random.Range(
                            0.0,
                            0.08),
                    .inclinationDegrees =
                        random.Range(
                            0.0,
                            8.0),
                    .centralMuM3PerS2 =
                        planetMu,
                    .synchronousRotation = true
                };

                result.moons.push_back(
                    CreateMoonInternal(
                        planetId,
                        moon));
            }
        }

        commands_.CommitTransaction();
        return result;
    }
    catch (...)
    {
        if (commands_.HasActiveTransaction())
        {
            commands_.RollbackTransaction();
        }
        throw;
    }
}
} // namespace orbit::editor_model

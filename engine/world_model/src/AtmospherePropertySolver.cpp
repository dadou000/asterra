#include <orbit/world_model/AtmospherePropertySolver.hpp>

#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/PropertyProvenanceStore.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <string>
#include <variant>

namespace orbit::world_model
{
namespace
{
constexpr f64 kUniversalGasConstant =
    8.31446261815324;
constexpr f64 kGravitationalConstant =
    6.67430e-11;

constexpr math::Double3 kEarthRayleigh{
    5.802e-6,
    13.558e-6,
    33.1e-6
};

constexpr math::Double3 kEarthAbsorption{
    0.650e-6,
    1.881e-6,
    0.085e-6
};

struct Preset
{
    std::string_view name;
    f64 pressurePascals;
    f64 temperatureKelvin;
    f64 nitrogen;
    f64 oxygen;
    f64 argon;
    f64 carbonDioxide;
    f64 aerosolOpticalDepth550;
    f64 aerosolSingleScatteringAlbedo;
    f64 aerosolAngstromExponent;
    f64 aerosolScaleHeightMeters;
    f64 absorberScale;
};

constexpr std::array<Preset, 4> kPresets{{
    {
        .name = "Earth-like",
        .pressurePascals = 101325.0,
        .temperatureKelvin = 288.15,
        .nitrogen = 0.78084,
        .oxygen = 0.20946,
        .argon = 0.00934,
        .carbonDioxide = 0.00036,
        .aerosolOpticalDepth550 = 0.005328,
        .aerosolSingleScatteringAlbedo = 0.90,
        .aerosolAngstromExponent = 1.3,
        .aerosolScaleHeightMeters = 1200.0,
        .absorberScale = 1.0
    },
    {
        .name = "Thin CO2",
        .pressurePascals = 9000.0,
        .temperatureKelvin = 235.0,
        .nitrogen = 0.02,
        .oxygen = 0.0,
        .argon = 0.01,
        .carbonDioxide = 0.97,
        .aerosolOpticalDepth550 = 0.015,
        .aerosolSingleScatteringAlbedo = 0.82,
        .aerosolAngstromExponent = 0.4,
        .aerosolScaleHeightMeters = 8000.0,
        .absorberScale = 0.05
    },
    {
        .name = "Dense CO2",
        .pressurePascals = 5.0e6,
        .temperatureKelvin = 650.0,
        .nitrogen = 0.025,
        .oxygen = 0.0,
        .argon = 0.005,
        .carbonDioxide = 0.97,
        .aerosolOpticalDepth550 = 0.20,
        .aerosolSingleScatteringAlbedo = 0.93,
        .aerosolAngstromExponent = 0.2,
        .aerosolScaleHeightMeters = 6000.0,
        .absorberScale = 0.15
    },
    {
        .name = "Dry Nitrogen",
        .pressurePascals = 80000.0,
        .temperatureKelvin = 270.0,
        .nitrogen = 0.97,
        .oxygen = 0.01,
        .argon = 0.019,
        .carbonDioxide = 0.001,
        .aerosolOpticalDepth550 = 0.002,
        .aerosolSingleScatteringAlbedo = 0.88,
        .aerosolAngstromExponent = 1.0,
        .aerosolScaleHeightMeters = 1500.0,
        .absorberScale = 0.05
    }
}};

[[nodiscard]] const Preset& RequirePreset(
    const std::string_view name)
{
    const auto found =
        std::find_if(
            kPresets.begin(),
            kPresets.end(),
            [name](const Preset& preset)
            {
                return preset.name == name;
            });

    if (found == kPresets.end())
    {
        throw std::invalid_argument(
            "Unknown atmosphere preset.");
    }

    return *found;
}

template <typename T>
[[nodiscard]] T PropertyOr(
    const scene::ObjectStore& objects,
    const scene::ObjectId object,
    const schema::PropertyId property,
    T fallback)
{
    const auto stored =
        objects.GetProperty(
            object,
            property);

    if (!stored.has_value())
    {
        return fallback;
    }

    const auto* typed =
        std::get_if<T>(
            &*stored);

    if (typed == nullptr)
    {
        throw std::runtime_error(
            "Atmosphere solver property has unexpected type.");
    }

    return *typed;
}

[[nodiscard]] bool Near(
    const f64 a,
    const f64 b) noexcept
{
    const f64 scale =
        std::max({
            1.0,
            std::abs(a),
            std::abs(b)
        });

    return
        std::abs(a - b) <=
        1.0e-10 * scale;
}

[[nodiscard]] bool Near(
    const math::Double3 a,
    const math::Double3 b) noexcept
{
    return
        Near(a.x, b.x) &&
        Near(a.y, b.y) &&
        Near(a.z, b.z);
}

template <typename T>
void DeriveProperty(
    scene::ObjectStore& objects,
    commands::CommandService& commands,
    const scene::ObjectId atmosphere,
    const schema::PropertyId target,
    const T& predicted,
    std::string explanation,
    AtmosphereSolveReport& report)
{
    const auto provenance =
        EffectivePropertyProvenance(
            objects,
            atmosphere,
            target);

    const auto stored =
        objects.GetProperty(
            atmosphere,
            target);

    bool matches = false;

    if (stored.has_value())
    {
        if (const auto* current =
                std::get_if<T>(
                    &*stored);
            current != nullptr)
        {
            matches =
                Near(
                    *current,
                    predicted);
        }
    }

    if (!CanSolverWrite(
            provenance))
    {
        if (!matches)
        {
            report.events.push_back({
                .target = target,
                .outcome =
                    AtmosphereSolveOutcome::
                        Conflict,
                .explanation =
                    "Expert/imported/locked value disagrees with the atmosphere solver; authoritative value was preserved."
            });
        }

        return;
    }

    if (!matches)
    {
        commands.SetProperty(
            atmosphere,
            target,
            predicted);
    }

    PropertyProvenance derived{
        .sourceMode =
            PropertySourceMode::Derived,
        .solveState =
            PropertySolveState::Solved,
        .sourceProperty =
            explanation
    };

    static_cast<void>(
        WritePropertyProvenance(
            objects,
            commands,
            atmosphere,
            target,
            derived));

    if (!matches)
    {
        report.events.push_back({
            .target = target,
            .outcome =
                AtmosphereSolveOutcome::
                    Derived,
            .explanation =
                std::move(explanation)
        });
    }
}

template <>
void DeriveProperty<math::Double3>(
    scene::ObjectStore& objects,
    commands::CommandService& commands,
    const scene::ObjectId atmosphere,
    const schema::PropertyId target,
    const math::Double3& predicted,
    std::string explanation,
    AtmosphereSolveReport& report)
{
    const auto provenance =
        EffectivePropertyProvenance(
            objects,
            atmosphere,
            target);

    const auto stored =
        objects.GetProperty(
            atmosphere,
            target);

    bool matches = false;

    if (stored.has_value())
    {
        if (const auto* current =
                std::get_if<math::Double3>(
                    &*stored);
            current != nullptr)
        {
            matches =
                Near(
                    *current,
                    predicted);
        }
    }

    if (!CanSolverWrite(
            provenance))
    {
        if (!matches)
        {
            report.events.push_back({
                .target = target,
                .outcome =
                    AtmosphereSolveOutcome::
                        Conflict,
                .explanation =
                    "Expert/imported/locked vector disagrees with the atmosphere solver; authoritative value was preserved."
            });
        }

        return;
    }

    if (!matches)
    {
        commands.SetProperty(
            atmosphere,
            target,
            predicted);
    }

    PropertyProvenance derived{
        .sourceMode =
            PropertySourceMode::Derived,
        .solveState =
            PropertySolveState::Solved,
        .sourceProperty =
            explanation
    };

    static_cast<void>(
        WritePropertyProvenance(
            objects,
            commands,
            atmosphere,
            target,
            derived));

    if (!matches)
    {
        report.events.push_back({
            .target = target,
            .outcome =
                AtmosphereSolveOutcome::
                    Derived,
            .explanation =
                std::move(explanation)
        });
    }
}

void WritePresetInput(
    scene::ObjectStore& objects,
    commands::CommandService& commands,
    const scene::ObjectId atmosphere,
    const schema::PropertyId property,
    const f64 value,
    const std::string_view preset)
{
    commands.SetProperty(
        atmosphere,
        property,
        value);

    static_cast<void>(
        WritePropertyProvenance(
            objects,
            commands,
            atmosphere,
            property,
            PropertyProvenance{
                .sourceMode =
                    PropertySourceMode::
                        Procedural,
                .solveState =
                    PropertySolveState::
                        Solved,
                .sourceProperty =
                    "Atmosphere preset: " +
                    std::string(preset)
            }));
}

[[nodiscard]] math::Double3 Scale(
    const math::Double3 value,
    const f64 scale) noexcept
{
    return value * scale;
}
} // namespace

bool AtmosphereSolveReport::HasConflict() const noexcept
{
    return std::any_of(
        events.begin(),
        events.end(),
        [](const AtmosphereSolveEvent& event)
        {
            return event.outcome ==
                AtmosphereSolveOutcome::
                    Conflict;
        });
}

bool AtmosphereSolveReport::HasInvalidInput() const noexcept
{
    return std::any_of(
        events.begin(),
        events.end(),
        [](const AtmosphereSolveEvent& event)
        {
            return event.outcome ==
                AtmosphereSolveOutcome::
                    InvalidInput;
        });
}

u32 AtmosphereSolveReport::DerivedCount() const noexcept
{
    return static_cast<u32>(
        std::count_if(
            events.begin(),
            events.end(),
            [](const AtmosphereSolveEvent& event)
            {
                return event.outcome ==
                    AtmosphereSolveOutcome::
                        Derived;
            }));
}

AtmospherePropertySolver::AtmospherePropertySolver(
    scene::ObjectStore& objects,
    commands::CommandService& commands)
    : objects_(objects),
      commands_(commands)
{
}

std::vector<std::string_view>
AtmospherePropertySolver::Presets()
{
    std::vector<std::string_view> result;
    result.reserve(
        kPresets.size());

    for (const auto& preset :
         kPresets)
    {
        result.push_back(
            preset.name);
    }

    return result;
}

AtmosphereSolveReport
AtmospherePropertySolver::ApplyPreset(
    const scene::ObjectId atmosphere,
    const std::string_view presetName)
{
    const auto& preset =
        RequirePreset(
            presetName);

    const bool ownsTransaction =
        !commands_.HasActiveTransaction();

    if (ownsTransaction)
    {
        commands_.BeginTransaction(
            "Apply Atmosphere Preset");
    }

    try
    {
        commands_.SetProperty(
            atmosphere,
            kAtmosphereAuthoringMode,
            std::string{
                "Derived Composition"});
        commands_.SetProperty(
            atmosphere,
            kAtmospherePreset,
            std::string{
                preset.name});

        WritePresetInput(
            objects_,
            commands_,
            atmosphere,
            kAtmosphereSurfacePressurePascals,
            preset.pressurePascals,
            preset.name);
        WritePresetInput(
            objects_,
            commands_,
            atmosphere,
            kAtmosphereSurfaceTemperatureKelvin,
            preset.temperatureKelvin,
            preset.name);
        WritePresetInput(
            objects_,
            commands_,
            atmosphere,
            kAtmosphereNitrogenFraction,
            preset.nitrogen,
            preset.name);
        WritePresetInput(
            objects_,
            commands_,
            atmosphere,
            kAtmosphereOxygenFraction,
            preset.oxygen,
            preset.name);
        WritePresetInput(
            objects_,
            commands_,
            atmosphere,
            kAtmosphereArgonFraction,
            preset.argon,
            preset.name);
        WritePresetInput(
            objects_,
            commands_,
            atmosphere,
            kAtmosphereCarbonDioxideFraction,
            preset.carbonDioxide,
            preset.name);
        WritePresetInput(
            objects_,
            commands_,
            atmosphere,
            kAtmosphereAerosolOpticalDepth550,
            preset.aerosolOpticalDepth550,
            preset.name);
        WritePresetInput(
            objects_,
            commands_,
            atmosphere,
            kAtmosphereAerosolSingleScatteringAlbedo,
            preset.aerosolSingleScatteringAlbedo,
            preset.name);
        WritePresetInput(
            objects_,
            commands_,
            atmosphere,
            kAtmosphereAerosolAngstromExponent,
            preset.aerosolAngstromExponent,
            preset.name);
        WritePresetInput(
            objects_,
            commands_,
            atmosphere,
            kAtmosphereAerosolScaleHeightMeters,
            preset.aerosolScaleHeightMeters,
            preset.name);
        WritePresetInput(
            objects_,
            commands_,
            atmosphere,
            kAtmosphereAbsorberScale,
            preset.absorberScale,
            preset.name);

        auto report =
            SolveInternal(
                atmosphere);

        if (ownsTransaction)
        {
            commands_.CommitTransaction();
        }

        return report;
    }
    catch (...)
    {
        if (ownsTransaction &&
            commands_.HasActiveTransaction())
        {
            commands_.RollbackTransaction();
        }

        throw;
    }
}

AtmosphereSolveReport
AtmospherePropertySolver::Solve(
    const scene::ObjectId atmosphere)
{
    const bool ownsTransaction =
        !commands_.HasActiveTransaction();

    if (ownsTransaction)
    {
        commands_.BeginTransaction(
            "Solve Atmosphere");
    }

    try
    {
        auto report =
            SolveInternal(
                atmosphere);

        if (ownsTransaction)
        {
            commands_.CommitTransaction();
        }

        return report;
    }
    catch (...)
    {
        if (ownsTransaction &&
            commands_.HasActiveTransaction())
        {
            commands_.RollbackTransaction();
        }

        throw;
    }
}

AtmosphereSolveReport
AtmospherePropertySolver::SolveInternal(
    const scene::ObjectId atmosphere)
{
    AtmosphereSolveReport report;

    const auto record =
        objects_.Find(
            atmosphere);

    if (!record.has_value() ||
        record->type !=
            kAtmosphereCapabilityType ||
        !record->parent.has_value())
    {
        throw std::invalid_argument(
            "Atmosphere solver requires an Atmosphere capability attached to a body.");
    }

    const auto body =
        objects_.Find(
            *record->parent);

    if (!body.has_value() ||
        body->type !=
            kCelestialBodyType)
    {
        throw std::invalid_argument(
            "Atmosphere capability must be attached directly to a Celestial Body.");
    }

    const std::string mode =
        PropertyOr<std::string>(
            objects_,
            atmosphere,
            kAtmosphereAuthoringMode,
            std::string{
                "Derived Composition"});

    if (mode ==
        "Expert Coefficients")
    {
        report.events.push_back({
            .target = {},
            .outcome =
                AtmosphereSolveOutcome::
                    NoChange,
            .explanation =
                "Expert Coefficients mode leaves M21 runtime coefficients under direct author authority."
        });
        return report;
    }

    if (mode !=
        "Derived Composition")
    {
        report.events.push_back({
            .target =
                kAtmosphereAuthoringMode,
            .outcome =
                AtmosphereSolveOutcome::
                    InvalidInput,
            .explanation =
                "Authoring Mode must be 'Derived Composition' or 'Expert Coefficients'."
        });
        return report;
    }

    const f64 pressure =
        PropertyOr<f64>(
            objects_,
            atmosphere,
            kAtmosphereSurfacePressurePascals,
            101325.0);
    const f64 temperature =
        PropertyOr<f64>(
            objects_,
            atmosphere,
            kAtmosphereSurfaceTemperatureKelvin,
            288.15);

    const f64 n2 =
        PropertyOr<f64>(
            objects_,
            atmosphere,
            kAtmosphereNitrogenFraction,
            0.78084);
    const f64 o2 =
        PropertyOr<f64>(
            objects_,
            atmosphere,
            kAtmosphereOxygenFraction,
            0.20946);
    const f64 ar =
        PropertyOr<f64>(
            objects_,
            atmosphere,
            kAtmosphereArgonFraction,
            0.00934);
    const f64 co2 =
        PropertyOr<f64>(
            objects_,
            atmosphere,
            kAtmosphereCarbonDioxideFraction,
            0.00036);

    const f64 fractionTotal =
        n2 + o2 + ar + co2;

    const f64 bodyRadius =
        PropertyOr<f64>(
            objects_,
            body->id,
            kBodyRadius,
            0.0);
    const f64 bodyMass =
        PropertyOr<f64>(
            objects_,
            body->id,
            kBodyMass,
            0.0);

    if (!std::isfinite(pressure) ||
        pressure <= 0.0 ||
        !std::isfinite(temperature) ||
        temperature <= 0.0 ||
        !std::isfinite(fractionTotal) ||
        fractionTotal <= 0.0 ||
        !std::isfinite(bodyRadius) ||
        bodyRadius <= 0.0)
    {
        report.events.push_back({
            .target = {},
            .outcome =
                AtmosphereSolveOutcome::
                    InvalidInput,
            .explanation =
                "Pressure, temperature, body radius and composition total must be finite and positive."
        });
        return report;
    }

    const f64 nn2 =
        n2 / fractionTotal;
    const f64 no2 =
        o2 / fractionTotal;
    const f64 nar =
        ar / fractionTotal;
    const f64 nco2 =
        co2 / fractionTotal;

    // kg/mol
    const f64 meanMolarMass =
        nn2 * 0.0280134 +
        no2 * 0.0319988 +
        nar * 0.039948 +
        nco2 * 0.0440095;

    f64 gravity =
        PropertyOr<f64>(
            objects_,
            atmosphere,
            kAtmosphereSurfaceGravityMetersPerSecondSquared,
            9.80665);

    const auto gravityProvenance =
        EffectivePropertyProvenance(
            objects_,
            atmosphere,
            kAtmosphereSurfaceGravityMetersPerSecondSquared);

    if (std::isfinite(bodyMass) &&
        bodyMass > 0.0 &&
        CanSolverWrite(
            gravityProvenance))
    {
        gravity =
            kGravitationalConstant *
            bodyMass /
            (bodyRadius *
             bodyRadius);

        DeriveProperty(
            objects_,
            commands_,
            atmosphere,
            kAtmosphereSurfaceGravityMetersPerSecondSquared,
            gravity,
            "surface gravity = G * body mass / body radius^2",
            report);
    }

    if (!std::isfinite(gravity) ||
        gravity <= 0.0 ||
        !std::isfinite(meanMolarMass) ||
        meanMolarMass <= 0.0)
    {
        report.events.push_back({
            .target =
                kAtmosphereSurfaceGravityMetersPerSecondSquared,
            .outcome =
                AtmosphereSolveOutcome::
                    InvalidInput,
            .explanation =
                "Surface gravity and mean molar mass must be finite and positive."
        });
        return report;
    }

    const f64 rayleighScaleHeight =
        kUniversalGasConstant *
        temperature /
        (meanMolarMass *
         gravity);

    const f64 numberDensityScale =
        (pressure / 101325.0) *
        (288.15 / temperature);

    // Approximate composition refractivity weighting around the Earth-like
    // baseline. Exact spectral refractive-index authoring remains available
    // through Expert Coefficients.
    const f64 compositionFactor =
        nn2 * 1.000 +
        no2 * 1.100 +
        nar * 0.950 +
        nco2 * 1.450;

    constexpr f64 earthCompositionFactor =
        0.78084 * 1.000 +
        0.20946 * 1.100 +
        0.00934 * 0.950 +
        0.00036 * 1.450;

    const f64 rayleighScale =
        numberDensityScale *
        compositionFactor /
        earthCompositionFactor;

    const math::Double3 rayleigh =
        Scale(
            kEarthRayleigh,
            rayleighScale);

    const f64 aerosolOpticalDepth =
        PropertyOr<f64>(
            objects_,
            atmosphere,
            kAtmosphereAerosolOpticalDepth550,
            0.005328);
    const f64 aerosolSingleScatteringAlbedo =
        PropertyOr<f64>(
            objects_,
            atmosphere,
            kAtmosphereAerosolSingleScatteringAlbedo,
            0.90);
    const f64 angstrom =
        PropertyOr<f64>(
            objects_,
            atmosphere,
            kAtmosphereAerosolAngstromExponent,
            1.3);
    const f64 mieScaleHeight =
        PropertyOr<f64>(
            objects_,
            atmosphere,
            kAtmosphereAerosolScaleHeightMeters,
            1200.0);

    if (!std::isfinite(
            aerosolOpticalDepth) ||
        aerosolOpticalDepth < 0.0 ||
        !std::isfinite(
            aerosolSingleScatteringAlbedo) ||
        aerosolSingleScatteringAlbedo < 0.0 ||
        aerosolSingleScatteringAlbedo > 1.0 ||
        !std::isfinite(angstrom) ||
        angstrom < 0.0 ||
        !std::isfinite(mieScaleHeight) ||
        mieScaleHeight <= 0.0)
    {
        report.events.push_back({
            .target =
                kAtmosphereAerosolOpticalDepth550,
            .outcome =
                AtmosphereSolveOutcome::
                    InvalidInput,
            .explanation =
                "Aerosol optical depth, single-scattering albedo, Angstrom exponent and scale height are invalid."
        });
        return report;
    }

    constexpr std::array<f64, 3>
        wavelengthsNm{
            680.0,
            550.0,
            440.0
        };

    math::Double3 mieExtinction{};

    f64* mieChannels[] = {
        &mieExtinction.x,
        &mieExtinction.y,
        &mieExtinction.z
    };

    for (std::size_t index = 0;
         index < wavelengthsNm.size();
         ++index)
    {
        const f64 spectralAod =
            aerosolOpticalDepth *
            std::pow(
                wavelengthsNm[index] /
                    550.0,
                -angstrom);

        *mieChannels[index] =
            spectralAod /
            mieScaleHeight;
    }

    const math::Double3 mieScattering =
        mieExtinction *
        aerosolSingleScatteringAlbedo;

    const f64 absorberScale =
        PropertyOr<f64>(
            objects_,
            atmosphere,
            kAtmosphereAbsorberScale,
            1.0);

    if (!std::isfinite(absorberScale) ||
        absorberScale < 0.0)
    {
        report.events.push_back({
            .target =
                kAtmosphereAbsorberScale,
            .outcome =
                AtmosphereSolveOutcome::
                    InvalidInput,
            .explanation =
                "Absorber column scale must be finite and non-negative."
        });
        return report;
    }

    const math::Double3 absorption =
        Scale(
            kEarthAbsorption,
            absorberScale);

    const f64 absorptionCenter =
        3.125 *
        rayleighScaleHeight;
    const f64 absorptionHalfWidth =
        1.875 *
        rayleighScaleHeight;

    const f64 atmosphereThickness =
        std::max({
            10.0 *
                rayleighScaleHeight,
            12.0 *
                mieScaleHeight,
            absorptionCenter +
                2.0 *
                absorptionHalfWidth,
            1.0
        });

    const f64 topRadius =
        bodyRadius +
        atmosphereThickness;

    DeriveProperty(
        objects_,
        commands_,
        atmosphere,
        kAtmosphereTopRadiusMeters,
        topRadius,
        "top radius = body radius + max(10 Rayleigh scale heights, 12 aerosol scale heights, absorber extent)",
        report);

    DeriveProperty(
        objects_,
        commands_,
        atmosphere,
        kAtmosphereRayleighScatteringPerMeter,
        rayleigh,
        "Rayleigh RGB scattering derived from pressure/temperature number density and bulk-gas refractivity weighting",
        report);

    DeriveProperty(
        objects_,
        commands_,
        atmosphere,
        kAtmosphereRayleighScaleHeightMeters,
        rayleighScaleHeight,
        "Rayleigh scale height = R * surface temperature / (mean molar mass * surface gravity)",
        report);

    DeriveProperty(
        objects_,
        commands_,
        atmosphere,
        kAtmosphereMieExtinctionPerMeter,
        mieExtinction,
        "Mie RGB extinction = spectral aerosol optical depth / aerosol scale height",
        report);

    DeriveProperty(
        objects_,
        commands_,
        atmosphere,
        kAtmosphereMieScatteringPerMeter,
        mieScattering,
        "Mie scattering = Mie extinction * aerosol single-scattering albedo",
        report);

    DeriveProperty(
        objects_,
        commands_,
        atmosphere,
        kAtmosphereMieScaleHeightMeters,
        mieScaleHeight,
        "Mie scale height follows authored aerosol scale height",
        report);

    DeriveProperty(
        objects_,
        commands_,
        atmosphere,
        kAtmosphereMieAnisotropy,
        0.8,
        "Mie anisotropy baseline; use Expert Coefficients to author an exact phase asymmetry",
        report);

    DeriveProperty(
        objects_,
        commands_,
        atmosphere,
        kAtmosphereAbsorptionExtinctionPerMeter,
        absorption,
        "Absorption extinction = baseline absorber spectral profile * absorber column scale",
        report);

    DeriveProperty(
        objects_,
        commands_,
        atmosphere,
        kAtmosphereAbsorptionCenterHeightMeters,
        absorptionCenter,
        "Absorber layer center = 3.125 Rayleigh scale heights",
        report);

    DeriveProperty(
        objects_,
        commands_,
        atmosphere,
        kAtmosphereAbsorptionHalfWidthMeters,
        absorptionHalfWidth,
        "Absorber layer half width = 1.875 Rayleigh scale heights",
        report);

    return report;
}
} // namespace orbit::world_model

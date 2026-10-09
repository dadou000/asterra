#include <orbit/terrain_impacts/ImpactField.hpp>

#include <toml++/toml.hpp>

#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

namespace orbit::terrain_impacts
{
namespace
{
template <typename Id>
[[nodiscard]] Id ParseId(
    const std::string& text,
    const std::string_view field)
{
    const auto parsed = Id::Parse(text);
    if (!parsed.has_value() || !parsed->IsValid())
    {
        throw std::runtime_error(
            "Impact field '" + std::string(field) +
            "' is not a valid stable Orbit ID.");
    }
    return *parsed;
}

[[nodiscard]] std::string RequiredString(
    const toml::table& table,
    const std::string_view key)
{
    const auto value = table[key].value<std::string>();
    if (!value.has_value() || value->empty())
    {
        throw std::runtime_error(
            "Impact field is missing non-empty string '" +
            std::string(key) + "'.");
    }
    return *value;
}

[[nodiscard]] f64 OptionalFloat(
    const toml::table& table,
    const std::string_view key,
    const f64 fallback)
{
    return table[key].value<f64>().value_or(fallback);
}

[[nodiscard]] bool OptionalBool(
    const toml::table& table,
    const std::string_view key,
    const bool fallback)
{
    return table[key].value<bool>().value_or(fallback);
}

[[nodiscard]] std::string OptionalString(
    const toml::table& table,
    const std::string_view key,
    std::string fallback)
{
    return table[key].value<std::string>().
        value_or(std::move(fallback));
}

[[nodiscard]] u32 OptionalU32(
    const toml::table& table,
    const std::string_view key,
    const u32 fallback)
{
    const auto value = table[key].value<i64>();
    if (!value.has_value())
    {
        return fallback;
    }

    if (*value < 0 ||
        static_cast<u64>(*value) >
            std::numeric_limits<u32>::max())
    {
        throw std::runtime_error(
            "Impact field '" + std::string(key) +
            "' is outside the 32-bit unsigned range.");
    }

    return static_cast<u32>(*value);
}

[[nodiscard]] u64 OptionalU64String(
    const toml::table& table,
    const std::string_view key,
    const u64 fallback)
{
    const auto text = table[key].value<std::string>();
    if (!text.has_value())
    {
        const auto integer = table[key].value<i64>();
        if (!integer.has_value())
        {
            return fallback;
        }
        if (*integer < 0)
        {
            throw std::runtime_error(
                "Impact field '" + std::string(key) +
                "' cannot be negative.");
        }
        return static_cast<u64>(*integer);
    }

    try
    {
        std::size_t consumed = 0;
        const u64 value =
            std::stoull(*text, &consumed, 10);
        if (consumed != text->size())
        {
            throw std::runtime_error("trailing");
        }
        return value;
    }
    catch (...)
    {
        throw std::runtime_error(
            "Impact field '" + std::string(key) +
            "' is not a valid unsigned integer.");
    }
}

[[nodiscard]] CraterProfileKind ParseProfile(
    const std::string& value)
{
    if (value == "auto") return CraterProfileKind::Auto;
    if (value == "simple") return CraterProfileKind::Simple;
    if (value == "complex") return CraterProfileKind::Complex;

    throw std::runtime_error(
        "Unknown crater profile kind: " + value);
}

[[nodiscard]] std::string_view ProfileName(
    const CraterProfileKind profile) noexcept
{
    switch (profile)
    {
    case CraterProfileKind::Auto: return "auto";
    case CraterProfileKind::Simple: return "simple";
    case CraterProfileKind::Complex: return "complex";
    }
    return "auto";
}

[[nodiscard]] SurfaceEnvironment ParseEnvironment(
    const std::string& value)
{
    if (value == "airless") return SurfaceEnvironment::Airless;
    if (value == "wet") return SurfaceEnvironment::Wet;
    if (value == "icy") return SurfaceEnvironment::Icy;
    if (value == "geologically_active") return SurfaceEnvironment::GeologicallyActive;
    throw std::runtime_error("Unknown surface environment: " + value);
}

[[nodiscard]] std::string_view EnvironmentName(
    const SurfaceEnvironment environment) noexcept
{
    switch (environment)
    {
    case SurfaceEnvironment::Airless: return "airless";
    case SurfaceEnvironment::Wet: return "wet";
    case SurfaceEnvironment::Icy: return "icy";
    case SurfaceEnvironment::GeologicallyActive: return "geologically_active";
    }
    return "airless";
}

[[nodiscard]] ResurfacingKind ParseResurfacingKind(const std::string& value)
{
    if (value == "lava_flow") return ResurfacingKind::LavaFlow;
    if (value == "ice_renewal") return ResurfacingKind::IceRenewal;
    if (value == "tectonic_renewal") return ResurfacingKind::TectonicRenewal;
    throw std::runtime_error("Unknown resurfacing kind: " + value);
}

[[nodiscard]] std::string_view ResurfacingKindName(const ResurfacingKind kind) noexcept
{
    switch (kind)
    {
    case ResurfacingKind::LavaFlow: return "lava_flow";
    case ResurfacingKind::IceRenewal: return "ice_renewal";
    case ResurfacingKind::TectonicRenewal: return "tectonic_renewal";
    }
    return "lava_flow";
}

[[nodiscard]] math::Double3 Direction(
    const toml::table& table)
{
    return {
        OptionalFloat(table, "center_x", 0.0),
        OptionalFloat(table, "center_y", 1.0),
        OptionalFloat(table, "center_z", 0.0)
    };
}
} // namespace

ImpactFieldDefinition ParseImpactFieldToml(
    const std::string_view text)
{
    const toml::table root =
        toml::parse(text);

    ImpactFieldDefinition result{
        .id = ParseId<ImpactFieldId>(
            RequiredString(root, "id"), "id"),
        .planet = ParseId<world::PlanetId>(
            RequiredString(root, "planet"), "planet"),
        .name = RequiredString(root, "name"),
        .seed = OptionalU64String(root, "seed", 0),
        .procedural = {},
        .complexTransitionRadiusMeters =
            OptionalFloat(
                root,
                "complex_transition_radius_m",
                18'000.0),
        .environment = ParseEnvironment(OptionalString(
            root, "environment", "airless")),
        .surfaceAgeYears = OptionalFloat(root, "surface_age_years", 0.0),
        .surfaceGravityMetersPerSecondSquared = OptionalFloat(
            root, "surface_gravity_mps2", 1.62),
        .targetDensityKgPerCubicMeter = OptionalFloat(
            root, "target_density_kg_m3", 2'700.0),
        .targetStrengthPascals = OptionalFloat(
            root, "target_strength_pa", 1'000'000.0)
    };

    if (const toml::table* procedural =
            root["procedural"].as_table())
    {
        result.procedural = {
            .count =
                OptionalU32(
                    *procedural,
                    "count",
                    0),
            .minimumRadiusMeters =
                OptionalFloat(
                    *procedural,
                    "minimum_radius_m",
                    1'000.0),
            .maximumRadiusMeters =
                OptionalFloat(
                    *procedural,
                    "maximum_radius_m",
                    100'000.0),
            .cumulativeExponent =
                OptionalFloat(
                    *procedural,
                    "cumulative_exponent",
                    2.0)
        };
    }

    if (const toml::array* impacts =
            root["impact"].as_array())
    {
        result.authoredImpacts.reserve(
            impacts->size());

        for (const toml::node& node : *impacts)
        {
            const toml::table* table =
                node.as_table();

            if (table == nullptr)
            {
                throw std::runtime_error(
                    "Impact entries must be TOML tables.");
            }

            result.authoredImpacts.push_back({
                .id = ParseId<ImpactId>(
                    RequiredString(*table, "id"),
                    "impact.id"),
                .centerUnitDirection =
                    Direction(*table),
                .radiusMeters =
                    OptionalFloat(
                        *table,
                        "radius_m",
                        -1.0),
                .profile =
                    ParseProfile(
                        OptionalString(
                            *table,
                            "profile",
                            "auto")),
                .simpleDepthRatio =
                    OptionalFloat(
                        *table,
                        "simple_depth_ratio",
                        0.18),
                .complexDepthRatio =
                    OptionalFloat(
                        *table,
                        "complex_depth_ratio",
                        0.075),
                .rimHeightRatio =
                    OptionalFloat(
                        *table,
                        "rim_height_ratio",
                        0.035),
                .ejectaThicknessRatio =
                    OptionalFloat(
                        *table,
                        "ejecta_thickness_ratio",
                        0.012),
                .ejectaExtentRadii =
                    OptionalFloat(
                        *table,
                        "ejecta_extent_radii",
                        3.0),
                .rayStrength =
                    OptionalFloat(
                        *table,
                        "ray_strength",
                        0.0),
                .rayCount =
                    OptionalU32(
                        *table,
                        "ray_count",
                        0),
                .rayExtentRadii = OptionalFloat(*table, "ray_extent_radii", 0.0),
                .rayIrregularity = OptionalFloat(*table, "ray_irregularity", 0.0),
                .degradation =
                    OptionalFloat(
                        *table,
                        "degradation",
                        0.0),
                .ageOrder =
                    OptionalU64String(
                        *table,
                        "age_order",
                        0),
                .formationAgeYears = OptionalFloat(*table, "formation_age_years", 0.0),
                .impactAngleDegrees = OptionalFloat(*table, "impact_angle_degrees", 0.0),
                .impactAzimuthRadians = OptionalFloat(*table, "impact_azimuth_radians", 0.0),
                .shapeIrregularity = OptionalFloat(*table, "shape_irregularity", 0.0),
                .meltFraction = OptionalFloat(*table, "melt_fraction", 0.0),
                .brecciaFraction = OptionalFloat(*table, "breccia_fraction", 0.0),
                .multiringStrength = OptionalFloat(*table, "multiring_strength", 0.0),
                .impactorDiameterMeters = OptionalFloat(*table, "impactor_diameter_m", 0.0),
                .impactVelocityMetersPerSecond = OptionalFloat(*table, "impact_velocity_mps", 0.0),
                .impactorDensityKgPerCubicMeter = OptionalFloat(
                    *table, "impactor_density_kg_m3", 0.0),
                .binarySeparationRadii = OptionalFloat(*table, "binary_separation_radii", 0.0),
                .binaryCompanionRadiusRatio = OptionalFloat(*table, "binary_companion_radius_ratio", 0.45),
                .binaryAzimuthRadians = OptionalFloat(*table, "binary_azimuth_radians", 0.0),
                .secondaryCount = OptionalU32(*table, "secondary_count", 0),
                .secondaryRadiusRatio = OptionalFloat(*table, "secondary_radius_ratio", 0.08),
                .secondaryRayAlignment = OptionalFloat(*table, "secondary_ray_alignment", 0.75),
                .enabled =
                    OptionalBool(
                        *table,
                        "enabled",
                        true),
                .authored = true
            });
        }
    }

    if (const toml::array* events = root["resurfacing"].as_array())
    {
        result.resurfacingEvents.reserve(events->size());
        for (const toml::node& node : *events)
        {
            const toml::table* table = node.as_table();
            if (table == nullptr)
                throw std::runtime_error("Resurfacing entries must be TOML tables.");
            const u32 count = OptionalU32(*table, "centerline_count", 0U);
            if (count > 4096U)
                throw std::runtime_error("Resurfacing centerline exceeds 4096 points.");
            ResurfacingRecord event{
                .id = ParseId<ImpactId>(RequiredString(*table, "id"), "resurfacing.id"),
                .kind = ParseResurfacingKind(OptionalString(*table, "kind", "lava_flow")),
                .widthMeters = OptionalFloat(*table, "width_m", 10'000.0),
                .thicknessMeters = OptionalFloat(*table, "thickness_m", 100.0),
                .formationAgeYears = OptionalFloat(*table, "formation_age_years", 0.0),
                .displacementUnitDirection = {
                    OptionalFloat(*table, "displacement_x", 0.0),
                    OptionalFloat(*table, "displacement_y", 0.0),
                    OptionalFloat(*table, "displacement_z", 0.0)},
                .displacementMeters = OptionalFloat(*table, "displacement_m", 0.0),
                .regionalPlateMotion = OptionalBool(*table, "plate_motion", false),
                .ageOrder = OptionalU64String(*table, "age_order", 0U),
                .enabled = OptionalBool(*table, "enabled", true)
            };
            event.centerlineUnitDirections.reserve(count);
            for (u32 point = 0U; point < count; ++point)
            {
                const std::string suffix = std::to_string(point);
                event.centerlineUnitDirections.push_back({
                    OptionalFloat(*table, "point" + suffix + "_x", 0.0),
                    OptionalFloat(*table, "point" + suffix + "_y", 0.0),
                    OptionalFloat(*table, "point" + suffix + "_z", 0.0)});
            }
            result.resurfacingEvents.push_back(std::move(event));
        }
    }

    if (const toml::table* ice = root["ice_fractures"].as_table())
    {
        result.iceFractures = std::make_shared<IceFractureDefinition>(
            IceFractureDefinition{
                .seed = OptionalU64String(*ice, "seed", 1U),
                .ageOrder = OptionalU64String(*ice, "age_order", 0U),
                .formationAgeYears = OptionalFloat(*ice, "formation_age_years", 0.0),
                .enabled = OptionalBool(*ice, "enabled", true),
                .tidalAxis = {
                    OptionalFloat(*ice, "tidal_axis_x", 1.0),
                    OptionalFloat(*ice, "tidal_axis_y", 0.0),
                    OptionalFloat(*ice, "tidal_axis_z", 0.0)},
                .spinAxis = {
                    OptionalFloat(*ice, "spin_axis_x", 0.0),
                    OptionalFloat(*ice, "spin_axis_y", 1.0),
                    OptionalFloat(*ice, "spin_axis_z", 0.0)},
                .tidalStress = OptionalFloat(*ice, "tidal_stress", 1.0),
                .rotationalStress = OptionalFloat(*ice, "rotational_stress", 0.2),
                .tensileStrength = OptionalFloat(*ice, "tensile_strength", 0.25),
                .fractureCount = OptionalU32(*ice, "fracture_count", 48U),
                .segmentsPerFracture = OptionalU32(*ice, "segments_per_fracture", 12U),
                .maximumLengthMeters = OptionalFloat(*ice, "maximum_length_m", 1'200'000.0),
                .widthMeters = OptionalFloat(*ice, "width_m", 1'800.0),
                .grooveDepthMeters = OptionalFloat(*ice, "groove_depth_m", 90.0),
                .ridgeHeightMeters = OptionalFloat(*ice, "ridge_height_m", 28.0),
                .branchProbability = OptionalFloat(*ice, "branch_probability", 0.18)
            });
    }

    if (!result.IsValid())
    {
        throw std::runtime_error(
            "Parsed M07 impact field is invalid.");
    }

    return result;
}

std::string SerializeImpactFieldToml(
    const ImpactFieldDefinition& definition)
{
    if (!definition.IsValid())
    {
        throw std::invalid_argument(
            "Cannot serialize invalid M07 impact field.");
    }

    toml::table root;
    root.insert("id", definition.id.ToString());
    root.insert("planet", definition.planet.ToString());
    root.insert("name", definition.name);
    root.insert("seed", std::to_string(definition.seed));
    root.insert(
        "complex_transition_radius_m",
        definition.complexTransitionRadiusMeters);
    root.insert("environment", std::string(EnvironmentName(definition.environment)));
    root.insert("surface_age_years", definition.surfaceAgeYears);
    root.insert("surface_gravity_mps2", definition.surfaceGravityMetersPerSecondSquared);
    root.insert("target_density_kg_m3", definition.targetDensityKgPerCubicMeter);
    root.insert("target_strength_pa", definition.targetStrengthPascals);

    toml::table procedural;
    procedural.insert(
        "count",
        static_cast<i64>(
            definition.procedural.count));
    procedural.insert(
        "minimum_radius_m",
        definition.procedural.minimumRadiusMeters);
    procedural.insert(
        "maximum_radius_m",
        definition.procedural.maximumRadiusMeters);
    procedural.insert(
        "cumulative_exponent",
        definition.procedural.cumulativeExponent);
    root.insert(
        "procedural",
        std::move(procedural));

    toml::array impacts;

    for (const ImpactRecord& impact :
         definition.authoredImpacts)
    {
        toml::table table;
        table.insert("id", impact.id.ToString());
        table.insert(
            "center_x",
            impact.centerUnitDirection.x);
        table.insert(
            "center_y",
            impact.centerUnitDirection.y);
        table.insert(
            "center_z",
            impact.centerUnitDirection.z);
        table.insert(
            "radius_m",
            impact.radiusMeters);
        table.insert(
            "profile",
            std::string(ProfileName(impact.profile)));
        table.insert(
            "simple_depth_ratio",
            impact.simpleDepthRatio);
        table.insert(
            "complex_depth_ratio",
            impact.complexDepthRatio);
        table.insert(
            "rim_height_ratio",
            impact.rimHeightRatio);
        table.insert(
            "ejecta_thickness_ratio",
            impact.ejectaThicknessRatio);
        table.insert(
            "ejecta_extent_radii",
            impact.ejectaExtentRadii);
        table.insert(
            "ray_strength",
            impact.rayStrength);
        table.insert(
            "ray_count",
            static_cast<i64>(
                impact.rayCount));
        table.insert("ray_extent_radii", impact.rayExtentRadii);
        table.insert("ray_irregularity", impact.rayIrregularity);
        table.insert(
            "degradation",
            impact.degradation);
        table.insert(
            "age_order",
            std::to_string(impact.ageOrder));
        table.insert("formation_age_years", impact.formationAgeYears);
        table.insert("impact_angle_degrees", impact.impactAngleDegrees);
        table.insert("impact_azimuth_radians", impact.impactAzimuthRadians);
        table.insert("shape_irregularity", impact.shapeIrregularity);
        table.insert("melt_fraction", impact.meltFraction);
        table.insert("breccia_fraction", impact.brecciaFraction);
        table.insert("multiring_strength", impact.multiringStrength);
        table.insert("impactor_diameter_m", impact.impactorDiameterMeters);
        table.insert("impact_velocity_mps", impact.impactVelocityMetersPerSecond);
        table.insert("impactor_density_kg_m3", impact.impactorDensityKgPerCubicMeter);
        table.insert("binary_separation_radii", impact.binarySeparationRadii);
        table.insert("binary_companion_radius_ratio", impact.binaryCompanionRadiusRatio);
        table.insert("binary_azimuth_radians", impact.binaryAzimuthRadians);
        table.insert("secondary_count", static_cast<i64>(impact.secondaryCount));
        table.insert("secondary_radius_ratio", impact.secondaryRadiusRatio);
        table.insert("secondary_ray_alignment", impact.secondaryRayAlignment);
        table.insert(
            "enabled",
            impact.enabled);

        impacts.push_back(
            std::move(table));
    }

    root.insert(
        "impact",
        std::move(impacts));

    toml::array resurfacing;
    for (const ResurfacingRecord& event : definition.resurfacingEvents)
    {
        toml::table table;
        table.insert("id", event.id.ToString());
        table.insert("kind", std::string(ResurfacingKindName(event.kind)));
        table.insert("width_m", event.widthMeters);
        table.insert("thickness_m", event.thicknessMeters);
        table.insert("formation_age_years", event.formationAgeYears);
        table.insert("displacement_x", event.displacementUnitDirection.x);
        table.insert("displacement_y", event.displacementUnitDirection.y);
        table.insert("displacement_z", event.displacementUnitDirection.z);
        table.insert("displacement_m", event.displacementMeters);
        table.insert("plate_motion", event.regionalPlateMotion);
        table.insert("age_order", std::to_string(event.ageOrder));
        table.insert("enabled", event.enabled);
        table.insert("centerline_count", static_cast<i64>(event.centerlineUnitDirections.size()));
        for (std::size_t point = 0U; point < event.centerlineUnitDirections.size(); ++point)
        {
            const std::string suffix = std::to_string(point);
            table.insert("point" + suffix + "_x", event.centerlineUnitDirections[point].x);
            table.insert("point" + suffix + "_y", event.centerlineUnitDirections[point].y);
            table.insert("point" + suffix + "_z", event.centerlineUnitDirections[point].z);
        }
        resurfacing.push_back(std::move(table));
    }
    root.insert("resurfacing", std::move(resurfacing));

    if (definition.iceFractures != nullptr)
    {
        const IceFractureDefinition& ice = *definition.iceFractures;
        toml::table table;
        table.insert("seed", std::to_string(ice.seed));
        table.insert("age_order", std::to_string(ice.ageOrder));
        table.insert("formation_age_years", ice.formationAgeYears);
        table.insert("enabled", ice.enabled);
        table.insert("tidal_axis_x", ice.tidalAxis.x);
        table.insert("tidal_axis_y", ice.tidalAxis.y);
        table.insert("tidal_axis_z", ice.tidalAxis.z);
        table.insert("spin_axis_x", ice.spinAxis.x);
        table.insert("spin_axis_y", ice.spinAxis.y);
        table.insert("spin_axis_z", ice.spinAxis.z);
        table.insert("tidal_stress", ice.tidalStress);
        table.insert("rotational_stress", ice.rotationalStress);
        table.insert("tensile_strength", ice.tensileStrength);
        table.insert("fracture_count", static_cast<i64>(ice.fractureCount));
        table.insert("segments_per_fracture", static_cast<i64>(ice.segmentsPerFracture));
        table.insert("maximum_length_m", ice.maximumLengthMeters);
        table.insert("width_m", ice.widthMeters);
        table.insert("groove_depth_m", ice.grooveDepthMeters);
        table.insert("ridge_height_m", ice.ridgeHeightMeters);
        table.insert("branch_probability", ice.branchProbability);
        root.insert("ice_fractures", std::move(table));
    }

    std::ostringstream stream;
    stream << root;
    return stream.str();
}

ImpactFieldDefinition LoadImpactFieldFile(
    const std::filesystem::path& path)
{
    std::ifstream stream(path);
    if (!stream)
    {
        throw std::runtime_error(
            "Unable to open M07 impact field: " +
            path.string());
    }

    std::ostringstream text;
    text << stream.rdbuf();

    return ParseImpactFieldToml(
        text.str());
}
} // namespace orbit::terrain_impacts

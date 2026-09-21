#pragma once

#include <orbit/commands/CommandService.hpp>
#include <orbit/scene/ObjectStore.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace orbit::editor_model
{
struct StarRecipe
{
    u64 seed{1};
    std::string name{"Star"};
    f64 massKilograms{1.98847e30};
    f64 radiusMeters{6.957e8};
    f64 effectiveTemperatureKelvin{5772.0};
    f64 rotationPeriodSeconds{2.192832e6};
};

struct RockyPlanetRecipe
{
    u64 seed{1};
    std::string name{"Rocky Planet"};
    f64 radiusMeters{6.371e6};
    f64 densityKilogramsPerCubicMeter{5514.0};
    f64 semiMajorAxisMeters{1.495978707e11};
    f64 eccentricity{0.0167};
    f64 inclinationDegrees{0.0};
    f64 centralMuM3PerS2{1.32712440018e20};
    bool atmosphere{true};
    bool ocean{true};
};

struct MoonRecipe
{
    u64 seed{1};
    std::string name{"Moon"};
    f64 radiusMeters{1.7374e6};
    f64 densityKilogramsPerCubicMeter{3344.0};
    f64 semiMajorAxisMeters{3.844e8};
    f64 eccentricity{0.0549};
    f64 inclinationDegrees{5.145};
    f64 centralMuM3PerS2{3.986004418e14};
    bool synchronousRotation{true};
};

struct SeededSystemRecipe
{
    u64 seed{1};
    std::string systemName{"Generated System"};
    std::string starName{"Primary"};
    u32 rockyPlanetCount{4};
    bool generateMoons{true};
};

struct SeededSystemResult
{
    scene::ObjectId system{};
    scene::ObjectId star{};
    std::vector<scene::ObjectId> planets;
    std::vector<scene::ObjectId> moons;
};

class CelestialRecipeService
{
public:
    CelestialRecipeService(
        scene::ObjectStore& objects,
        commands::CommandService& commands);

    [[nodiscard]] scene::ObjectId CreateStar(
        scene::ObjectId parent,
        const StarRecipe& recipe);

    [[nodiscard]] scene::ObjectId CreateRockyPlanet(
        scene::ObjectId parent,
        const RockyPlanetRecipe& recipe);

    [[nodiscard]] scene::ObjectId CreateMoon(
        scene::ObjectId parentBody,
        const MoonRecipe& recipe);

    [[nodiscard]] SeededSystemResult CreateSeededSystem(
        scene::ObjectId world,
        const SeededSystemRecipe& recipe);

private:
    [[nodiscard]] scene::ObjectId CreateStarInternal(
        scene::ObjectId parent,
        const StarRecipe& recipe);

    [[nodiscard]] scene::ObjectId CreateRockyPlanetInternal(
        scene::ObjectId parent,
        const RockyPlanetRecipe& recipe);

    [[nodiscard]] scene::ObjectId CreateMoonInternal(
        scene::ObjectId parentBody,
        const MoonRecipe& recipe);

    scene::ObjectStore& objects_;
    commands::CommandService& commands_;
};
} // namespace orbit::editor_model

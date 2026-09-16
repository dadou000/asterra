#include <orbit/paths/PathProfile.hpp>

#include <filesystem>
#include <iostream>

#define ORBIT_TEST_CHECK(expression) \
    do \
    { \
        if (!(expression)) \
        { \
            std::cerr << "Path profile test failed: " #expression \
                      << " at line " << __LINE__ << '\n'; \
            return 1; \
        } \
    } while (false)

int main()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        "orbit-path-profile-tests";
    const auto path =
        root /
        "Rail.orbitpathprofile";

    std::filesystem::remove_all(root);

    const orbit::paths::PathProfile source{
        .name = "Mainline Rail",
        .kind = orbit::paths::PathProfileKind::Rail,
        .widthMeters = 4.2,
        .lanes = 2,
        .minimumRadiusMeters = 650.0,
        .maximumGrade = 0.025,
        .allowBridge = true,
        .allowTunnel = true,
        .terrainCutCost = 2.0,
        .terrainFillCost = 3.0,
        .waterCrossingCost = 8.0,
        .preferredCostFields = {
            "terrain.slope",
            "terrain.buildability"
        }
    };

    orbit::paths::SavePathProfile(
        path,
        source);

    ORBIT_TEST_CHECK(
        std::filesystem::is_regular_file(path));

    const auto loaded =
        orbit::paths::LoadPathProfile(path);

    ORBIT_TEST_CHECK(loaded == source);

    bool rejected = false;
    try
    {
        auto invalid = source;
        invalid.maximumGrade = 1.5;
        orbit::paths::SavePathProfile(
            root / "Invalid.orbitpathprofile",
            invalid);
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }

    ORBIT_TEST_CHECK(rejected);

    std::filesystem::remove_all(root);
    return 0;
}

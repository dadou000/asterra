add_library(OrbitCelestialClouds STATIC
    src/CloudField.cpp
)

add_library(
    Orbit::CelestialClouds
    ALIAS OrbitCelestialClouds
)

target_include_directories(
    OrbitCelestialClouds
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
    OrbitCelestialClouds
    PUBLIC
        Orbit::Core
        Orbit::Math
        Orbit::Rhi
        Orbit::Terrain
        Orbit::Time
)

target_compile_features(
    OrbitCelestialClouds PUBLIC cxx_std_23)
orbit_enable_warnings(
    OrbitCelestialClouds)

if(BUILD_TESTING)
    add_executable(
        OrbitCelestialCloudTests
        tests/CloudFieldTests.cpp
    )

    target_link_libraries(
        OrbitCelestialCloudTests
        PRIVATE
            Orbit::CelestialClouds
    )

    target_compile_features(
        OrbitCelestialCloudTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitCelestialCloudTests)

    add_test(
        NAME Orbit.CelestialClouds
        COMMAND OrbitCelestialCloudTests
    )
endif()

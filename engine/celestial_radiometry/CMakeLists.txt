add_library(OrbitCelestialRadiometry STATIC
    src/Radiometry.cpp
)

add_library(
    Orbit::CelestialRadiometry
    ALIAS OrbitCelestialRadiometry
)

target_include_directories(
    OrbitCelestialRadiometry
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
    OrbitCelestialRadiometry
    PUBLIC
        Orbit::Core
        Orbit::Math
)

target_compile_features(
    OrbitCelestialRadiometry PUBLIC cxx_std_23)
orbit_enable_warnings(
    OrbitCelestialRadiometry)

if(BUILD_TESTING)
    add_executable(
        OrbitCelestialRadiometryTests
        tests/RadiometryTests.cpp
    )

    target_link_libraries(
        OrbitCelestialRadiometryTests
        PRIVATE
            Orbit::CelestialRadiometry
    )

    target_compile_features(
        OrbitCelestialRadiometryTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitCelestialRadiometryTests)

    add_test(
        NAME Orbit.CelestialRadiometry
        COMMAND OrbitCelestialRadiometryTests
    )
endif()

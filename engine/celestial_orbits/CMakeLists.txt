add_library(OrbitCelestialOrbits STATIC
    src/OrbitState.cpp
)

add_library(Orbit::CelestialOrbits ALIAS OrbitCelestialOrbits)

target_include_directories(OrbitCelestialOrbits
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitCelestialOrbits
    PUBLIC
        Orbit::Core
        Orbit::Math
        Orbit::Time
)

target_compile_features(OrbitCelestialOrbits PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitCelestialOrbits)

if(BUILD_TESTING)
    add_executable(OrbitAnalyticConicTests
        tests/AnalyticConicTests.cpp
    )

    target_link_libraries(OrbitAnalyticConicTests
        PRIVATE
            Orbit::CelestialOrbits
    )

    target_compile_features(
        OrbitAnalyticConicTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitAnalyticConicTests)

    add_test(
        NAME Orbit.AnalyticConics
        COMMAND OrbitAnalyticConicTests
    )
endif()

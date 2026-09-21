add_library(OrbitCelestialRepresentation STATIC
    src/RepresentationResolver.cpp
    src/RepresentationTracker.cpp
)

add_library(
    Orbit::CelestialRepresentation
    ALIAS OrbitCelestialRepresentation
)

target_include_directories(
    OrbitCelestialRepresentation
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
    OrbitCelestialRepresentation
    PUBLIC
        Orbit::Core
)

target_compile_features(
    OrbitCelestialRepresentation
    PUBLIC cxx_std_23)
orbit_enable_warnings(
    OrbitCelestialRepresentation)

if(BUILD_TESTING)
    add_executable(
        OrbitCelestialRepresentationTests
        tests/RepresentationResolverTests.cpp
    )

    target_link_libraries(
        OrbitCelestialRepresentationTests
        PRIVATE
            Orbit::CelestialRepresentation
    )

    target_compile_features(
        OrbitCelestialRepresentationTests
        PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitCelestialRepresentationTests)

    add_test(
        NAME Orbit.CelestialRepresentation
        COMMAND OrbitCelestialRepresentationTests
    )
endif()

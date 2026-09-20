add_library(OrbitWorldModel STATIC
    src/CelestialSchemas.cpp
    src/WorldSchemas.cpp
    src/UniverseComposition.cpp
)

add_library(Orbit::WorldModel ALIAS OrbitWorldModel)

target_include_directories(OrbitWorldModel
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitWorldModel
    PUBLIC
        Orbit::Core
        Orbit::Frames
        Orbit::Math
        Orbit::Scene
        Orbit::Schema
        Orbit::Time
        Orbit::Universe
)

target_compile_features(OrbitWorldModel PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitWorldModel)

if(BUILD_TESTING)
    add_executable(OrbitUniverseCompositionTests
        tests/UniverseCompositionTests.cpp
    )

    target_link_libraries(OrbitUniverseCompositionTests
        PRIVATE
            Orbit::Commands
            Orbit::Documents
            Orbit::WorldModel
    )

    target_compile_features(
        OrbitUniverseCompositionTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitUniverseCompositionTests)

    add_test(
        NAME Orbit.UniverseComposition
        COMMAND OrbitUniverseCompositionTests
    )

    add_executable(OrbitSurfaceAuthorityContractTests
        tests/SurfaceAuthorityContractTests.cpp
    )

    target_link_libraries(OrbitSurfaceAuthorityContractTests
        PRIVATE
            Orbit::WorldModel
    )

    target_compile_features(
        OrbitSurfaceAuthorityContractTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitSurfaceAuthorityContractTests)

    add_test(
        NAME Orbit.SurfaceAuthorityContracts
        COMMAND OrbitSurfaceAuthorityContractTests
    )

    add_executable(OrbitCelestialCapabilitySchemasTests
        tests/CelestialCapabilitySchemasTests.cpp
    )

    target_link_libraries(OrbitCelestialCapabilitySchemasTests
        PRIVATE
            Orbit::Commands
            Orbit::Documents
            Orbit::WorldModel
    )

    target_compile_features(
        OrbitCelestialCapabilitySchemasTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitCelestialCapabilitySchemasTests)

    add_test(
        NAME Orbit.CelestialCapabilitySchemas
        COMMAND OrbitCelestialCapabilitySchemasTests
    )
endif()

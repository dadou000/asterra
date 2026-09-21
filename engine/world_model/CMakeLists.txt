add_library(OrbitWorldModel STATIC
    src/CelestialSchemas.cpp
    src/CelestialRadiometryBinding.cpp
    src/CelestialLightingService.cpp
    src/CelestialAtmosphereBinding.cpp
    src/CelestialCloudBinding.cpp
    src/CelestialOceanBinding.cpp
    src/CelestialRingBinding.cpp
    src/CelestialGiantBinding.cpp
    src/CelestialSmallBodyBinding.cpp
    src/CelestialMagnetosphereBinding.cpp
    src/AtmospherePropertySolver.cpp
    src/PropertyProvenanceSchema.cpp
    src/PropertyProvenanceStore.cpp
    src/PropertyProvenance.cpp
    src/PhysicalPropertySolver.cpp
    src/WorldSchemas.cpp
    src/LocalLightBinding.cpp
    src/VisibilityProxyBinding.cpp
    src/UniverseComposition.cpp
)

add_library(Orbit::WorldModel ALIAS OrbitWorldModel)

target_include_directories(OrbitWorldModel
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitWorldModel
    PUBLIC
        Orbit::Commands
        Orbit::CelestialGravity
        Orbit::CelestialLighting
        Orbit::CelestialAtmosphere
        Orbit::CelestialClouds
        Orbit::CelestialOcean
        Orbit::CelestialRings
        Orbit::CelestialGiants
        Orbit::CelestialSmallBodies
        Orbit::CelestialMagnetosphere
        Orbit::CelestialStellar
        Orbit::CelestialOrbits
        Orbit::CelestialRadiometry
        Orbit::CelestialRotation
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

    add_executable(OrbitLocalLightBindingTests
        tests/LocalLightBindingTests.cpp
    )

    add_executable(OrbitVisibilityProxyBindingTests
        tests/VisibilityProxyBindingTests.cpp
    )

    add_executable(OrbitSurfaceAuthorityContractTests
        tests/SurfaceAuthorityContractTests.cpp
    )

    target_link_libraries(OrbitLocalLightBindingTests
        PRIVATE
            Orbit::Commands
            Orbit::Documents
            Orbit::WorldModel
    )

    target_link_libraries(OrbitVisibilityProxyBindingTests
        PRIVATE
            Orbit::Commands
            Orbit::Documents
            Orbit::WorldModel
    )
    target_compile_features(
        OrbitVisibilityProxyBindingTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitVisibilityProxyBindingTests)

    add_test(
        NAME Orbit.VisibilityProxyBinding
        COMMAND OrbitVisibilityProxyBindingTests
    )
    target_compile_features(
        OrbitLocalLightBindingTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitLocalLightBindingTests)

    add_test(
        NAME Orbit.LocalLightBinding
        COMMAND OrbitLocalLightBindingTests
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

    add_executable(OrbitPropertyProvenanceTests
        tests/PropertyProvenanceTests.cpp
    )

    target_link_libraries(OrbitPropertyProvenanceTests
        PRIVATE
            Orbit::WorldModel
    )

    target_compile_features(
        OrbitPropertyProvenanceTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitPropertyProvenanceTests)

    add_test(
        NAME Orbit.PropertyProvenance
        COMMAND OrbitPropertyProvenanceTests
    )

    add_executable(OrbitPhysicalPropertySolverTests
        tests/PhysicalPropertySolverTests.cpp
    )

    target_link_libraries(OrbitPhysicalPropertySolverTests
        PRIVATE
            Orbit::WorldModel
    )

    target_compile_features(
        OrbitPhysicalPropertySolverTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitPhysicalPropertySolverTests)

    add_test(
        NAME Orbit.PhysicalPropertySolver
        COMMAND OrbitPhysicalPropertySolverTests
    )

    add_executable(OrbitReferenceNodeCompositionTests
        tests/ReferenceNodeCompositionTests.cpp
    )

    target_link_libraries(OrbitReferenceNodeCompositionTests
        PRIVATE
            Orbit::Commands
            Orbit::Documents
            Orbit::WorldModel
    )

    target_compile_features(
        OrbitReferenceNodeCompositionTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitReferenceNodeCompositionTests)

    add_test(
        NAME Orbit.ReferenceNodeComposition
        COMMAND OrbitReferenceNodeCompositionTests
    )

    add_executable(OrbitAnalyticOrbitCompositionTests
        tests/AnalyticOrbitCompositionTests.cpp
    )

    target_link_libraries(OrbitAnalyticOrbitCompositionTests
        PRIVATE
            Orbit::Commands
            Orbit::Documents
            Orbit::WorldModel
    )

    target_compile_features(
        OrbitAnalyticOrbitCompositionTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitAnalyticOrbitCompositionTests)

    add_test(
        NAME Orbit.AnalyticOrbitComposition
        COMMAND OrbitAnalyticOrbitCompositionTests
    )

    add_executable(OrbitRotationCompositionTests
        tests/RotationCompositionTests.cpp
    )

    target_link_libraries(OrbitRotationCompositionTests
        PRIVATE
            Orbit::Commands
            Orbit::Documents
            Orbit::WorldModel
    )

    target_compile_features(
        OrbitRotationCompositionTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitRotationCompositionTests)

    add_test(
        NAME Orbit.RotationComposition
        COMMAND OrbitRotationCompositionTests
    )

    add_executable(OrbitImportedEphemerisCompositionTests
        tests/ImportedEphemerisCompositionTests.cpp
    )

    target_link_libraries(OrbitImportedEphemerisCompositionTests
        PRIVATE
            Orbit::Commands
            Orbit::Documents
            Orbit::WorldModel
    )

    target_compile_features(
        OrbitImportedEphemerisCompositionTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitImportedEphemerisCompositionTests)

    add_test(
        NAME Orbit.ImportedEphemerisComposition
        COMMAND OrbitImportedEphemerisCompositionTests
    )

    add_executable(OrbitNBodyPromotionCompositionTests
        tests/NBodyPromotionCompositionTests.cpp
    )

    target_link_libraries(OrbitNBodyPromotionCompositionTests
        PRIVATE
            Orbit::Commands
            Orbit::Documents
            Orbit::WorldModel
    )

    target_compile_features(
        OrbitNBodyPromotionCompositionTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitNBodyPromotionCompositionTests)

    add_test(
        NAME Orbit.NBodyPromotionComposition
        COMMAND OrbitNBodyPromotionCompositionTests
    )

    add_executable(OrbitGravityCompositionTests
        tests/GravityCompositionTests.cpp
    )

    target_link_libraries(OrbitGravityCompositionTests
        PRIVATE
            Orbit::Commands
            Orbit::Documents
            Orbit::WorldModel
    )

    target_compile_features(
        OrbitGravityCompositionTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitGravityCompositionTests)

    add_test(
        NAME Orbit.GravityComposition
        COMMAND OrbitGravityCompositionTests
    )

    add_executable(
        OrbitAtmospherePropertySolverTests
        tests/AtmospherePropertySolverTests.cpp
    )

    target_link_libraries(
        OrbitAtmospherePropertySolverTests
        PRIVATE
            Orbit::Commands
            Orbit::Documents
            Orbit::WorldModel
    )

    target_compile_features(
        OrbitAtmospherePropertySolverTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitAtmospherePropertySolverTests)

    add_test(
        NAME Orbit.AtmospherePropertySolver
        COMMAND OrbitAtmospherePropertySolverTests
    )

endif()


if(BUILD_TESTING)
    add_executable(
        OrbitCelestialLightingCompositionTests
        tests/CelestialLightingCompositionTests.cpp
    )

    target_link_libraries(
        OrbitCelestialLightingCompositionTests
        PRIVATE
            Orbit::Commands
            Orbit::Documents
            Orbit::WorldModel
    )

    target_compile_features(
        OrbitCelestialLightingCompositionTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitCelestialLightingCompositionTests)

    add_test(
        NAME Orbit.CelestialLightingComposition
        COMMAND OrbitCelestialLightingCompositionTests
    )
endif()


if(BUILD_TESTING)
    add_executable(
        OrbitCelestialAtmosphereBindingTests
        tests/CelestialAtmosphereBindingTests.cpp
    )

    target_link_libraries(
        OrbitCelestialAtmosphereBindingTests
        PRIVATE
            Orbit::Commands
            Orbit::Documents
            Orbit::WorldModel
    )

    target_compile_features(
        OrbitCelestialAtmosphereBindingTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitCelestialAtmosphereBindingTests)

    add_test(
        NAME Orbit.CelestialAtmosphereBinding
        COMMAND OrbitCelestialAtmosphereBindingTests
    )
endif()


if(BUILD_TESTING)
    add_executable(
        OrbitCelestialCloudCompositionTests
        tests/CelestialCloudCompositionTests.cpp
    )

    target_link_libraries(
        OrbitCelestialCloudCompositionTests
        PRIVATE
            Orbit::Commands
            Orbit::Documents
            Orbit::WorldModel
    )

    target_compile_features(
        OrbitCelestialCloudCompositionTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitCelestialCloudCompositionTests)

    add_test(
        NAME Orbit.CelestialCloudComposition
        COMMAND OrbitCelestialCloudCompositionTests
    )
endif()


if(BUILD_TESTING)
    add_executable(
        OrbitCelestialOceanBindingTests
        tests/CelestialOceanBindingTests.cpp
    )

    target_link_libraries(
        OrbitCelestialOceanBindingTests
        PRIVATE
            Orbit::Commands
            Orbit::Documents
            Orbit::WorldModel
    )

    target_compile_features(
        OrbitCelestialOceanBindingTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitCelestialOceanBindingTests)

    add_test(
        NAME Orbit.CelestialOceanBinding
        COMMAND OrbitCelestialOceanBindingTests
    )
endif()


if(BUILD_TESTING)
    add_executable(
        OrbitCelestialRingBindingTests
        tests/CelestialRingBindingTests.cpp
    )

    target_link_libraries(
        OrbitCelestialRingBindingTests
        PRIVATE
            Orbit::Commands
            Orbit::Documents
            Orbit::WorldModel
    )

    target_compile_features(
        OrbitCelestialRingBindingTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitCelestialRingBindingTests)

    add_test(
        NAME Orbit.CelestialRingBinding
        COMMAND OrbitCelestialRingBindingTests
    )
endif()


if(BUILD_TESTING)
    add_executable(
        OrbitCelestialStellarBindingTests
        tests/CelestialStellarBindingTests.cpp
    )

    target_link_libraries(
        OrbitCelestialStellarBindingTests
        PRIVATE
            Orbit::Commands
            Orbit::Documents
            Orbit::WorldModel
    )

    target_compile_features(
        OrbitCelestialStellarBindingTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitCelestialStellarBindingTests)

    add_test(
        NAME Orbit.CelestialStellarBinding
        COMMAND OrbitCelestialStellarBindingTests
    )
endif()


if(BUILD_TESTING)
    add_executable(
        OrbitCelestialGiantBindingTests
        tests/CelestialGiantBindingTests.cpp
    )

    target_link_libraries(
        OrbitCelestialGiantBindingTests
        PRIVATE
            Orbit::Commands
            Orbit::Documents
            Orbit::WorldModel
    )

    target_compile_features(
        OrbitCelestialGiantBindingTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitCelestialGiantBindingTests)

    add_test(
        NAME Orbit.CelestialGiantBinding
        COMMAND OrbitCelestialGiantBindingTests
    )
endif()


if(BUILD_TESTING)
    add_executable(
        OrbitCelestialSmallBodyBindingTests
        tests/CelestialSmallBodyBindingTests.cpp
    )

    target_link_libraries(
        OrbitCelestialSmallBodyBindingTests
        PRIVATE
            Orbit::Commands
            Orbit::Documents
            Orbit::WorldModel
    )

    target_compile_features(
        OrbitCelestialSmallBodyBindingTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitCelestialSmallBodyBindingTests)

    add_test(
        NAME Orbit.CelestialSmallBodyBinding
        COMMAND OrbitCelestialSmallBodyBindingTests
    )
endif()

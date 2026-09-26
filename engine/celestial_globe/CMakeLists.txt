add_library(OrbitCelestialGlobe STATIC
    src/MacroGlobe.cpp
    src/PlanetPatchHierarchy.cpp
    src/AdaptiveMacroGlobeRenderer.cpp
)

# Keep the proven macro-globe Vulkan pipelines as a private legacy renderer.
# AdaptiveMacroGlobeRenderer.cpp exposes the public renderer and drives those
# pipelines once per resident cube-sphere patch. MacroGlobeFingerprint is also
# wrapped so the renderer can associate a stable product fingerprint with the
# terrain authority used to build/refine its patches.
set_source_files_properties(
    src/MacroGlobe.cpp
    PROPERTIES
        COMPILE_DEFINITIONS
            "ORBIT_BUILD_LEGACY_MACRO_GLOBE_RENDERER=1;MacroGlobeRenderer=LegacyMacroGlobeRenderer;MacroGlobeFingerprint=LegacyMacroGlobeFingerprint"
)

add_library(
    Orbit::CelestialGlobe
    ALIAS OrbitCelestialGlobe
)

target_include_directories(
    OrbitCelestialGlobe
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
    OrbitCelestialGlobe
    PUBLIC
        Orbit::CelestialAppearance
        Orbit::Core
        Orbit::Math
        Orbit::RenderView
        Orbit::RHI
        Orbit::Shader
        Orbit::Terrain
        Orbit::Universe
)

target_compile_features(
    OrbitCelestialGlobe PUBLIC cxx_std_23)
orbit_enable_warnings(
    OrbitCelestialGlobe)

if(BUILD_TESTING)
    add_executable(
        OrbitMacroGlobeTests
        tests/MacroGlobeTests.cpp
    )

    target_link_libraries(
        OrbitMacroGlobeTests
        PRIVATE
            Orbit::CelestialGlobe
    )

    target_compile_features(
        OrbitMacroGlobeTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitMacroGlobeTests)

    add_test(
        NAME Orbit.MacroGlobe
        COMMAND OrbitMacroGlobeTests
    )

    add_executable(
        OrbitPlanetPatchHierarchyTests
        tests/PlanetPatchHierarchyTests.cpp
    )

    target_link_libraries(
        OrbitPlanetPatchHierarchyTests
        PRIVATE
            Orbit::CelestialGlobe
    )

    target_compile_features(
        OrbitPlanetPatchHierarchyTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitPlanetPatchHierarchyTests)

    add_test(
        NAME Orbit.PlanetPatchHierarchy
        COMMAND OrbitPlanetPatchHierarchyTests
    )
endif()

add_library(OrbitEditorModel STATIC
    src/ShortcutRegistry.cpp
    src/CommandSurfaces.cpp
    src/AuthoringCommands.cpp
    src/MaterialAuthoringCommands.cpp
    src/TerrainAuthoringCommands.cpp
    src/BuiltinSchemas.cpp
    src/CelestialAuthoringModel.cpp
    src/ExplorerModel.cpp
    src/InspectorModel.cpp
    src/OutputLog.cpp
    src/SurfaceAuthoringModel.cpp
    src/SystemViewModel.cpp
)

add_library(Orbit::EditorModel ALIAS OrbitEditorModel)

target_include_directories(OrbitEditorModel
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitEditorModel
    PUBLIC
        Orbit::Commands
        Orbit::Core
        Orbit::Documents
        Orbit::Paths
        Orbit::Platform
        Orbit::Scene
        Orbit::Schema
        Orbit::Selection
        Orbit::SurfaceAuthoring
        Orbit::SurfaceModel
        Orbit::TerrainBiome
        Orbit::WorldModel
)

target_compile_features(OrbitEditorModel PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitEditorModel)

if(BUILD_TESTING)
    add_executable(OrbitTerrainAuthoringCommandTests
        tests/TerrainAuthoringCommandTests.cpp
    )

    target_link_libraries(OrbitTerrainAuthoringCommandTests
        PRIVATE
            Orbit::EditorModel
    )

    target_compile_features(
        OrbitTerrainAuthoringCommandTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainAuthoringCommandTests)

    add_test(
        NAME Orbit.TerrainAuthoringCommand
        COMMAND OrbitTerrainAuthoringCommandTests
    )
endif()


if(BUILD_TESTING)
    add_executable(OrbitSurfaceAuthoringModelTests
        tests/SurfaceAuthoringModelTests.cpp
    )

    target_link_libraries(OrbitSurfaceAuthoringModelTests
        PRIVATE
            Orbit::EditorModel
    )

    target_compile_features(
        OrbitSurfaceAuthoringModelTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitSurfaceAuthoringModelTests)

    add_test(
        NAME Orbit.SurfaceAuthoringModel
        COMMAND OrbitSurfaceAuthoringModelTests
    )
endif()

if(BUILD_TESTING)
    add_executable(OrbitRockyPlanetAuthoringTests
        tests/RockyPlanetAuthoringTests.cpp
    )

    target_link_libraries(OrbitRockyPlanetAuthoringTests
        PRIVATE
            Orbit::EditorModel
    )

    target_compile_features(
        OrbitRockyPlanetAuthoringTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitRockyPlanetAuthoringTests)

    add_test(
        NAME Orbit.RockyPlanetAuthoring
        COMMAND OrbitRockyPlanetAuthoringTests
    )
endif()


if(BUILD_TESTING)
    add_executable(OrbitCelestialAuthoringModelTests
        tests/CelestialAuthoringModelTests.cpp
    )

    target_link_libraries(OrbitCelestialAuthoringModelTests
        PRIVATE
            Orbit::EditorModel
    )

    target_compile_features(
        OrbitCelestialAuthoringModelTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitCelestialAuthoringModelTests)

    add_test(
        NAME Orbit.CelestialAuthoringModel
        COMMAND OrbitCelestialAuthoringModelTests
    )
endif()

if(BUILD_TESTING)
    add_executable(OrbitSystemViewModelTests
        tests/SystemViewModelTests.cpp
    )

    target_link_libraries(OrbitSystemViewModelTests
        PRIVATE
            Orbit::EditorModel
    )

    target_compile_features(
        OrbitSystemViewModelTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitSystemViewModelTests)

    add_test(
        NAME Orbit.SystemViewModel
        COMMAND OrbitSystemViewModelTests
    )
endif()

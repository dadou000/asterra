add_executable(OrbitWorldTests
    WorldCoordinatesTests.cpp
)

target_link_libraries(OrbitWorldTests
    PRIVATE
        Orbit::World
)

target_compile_features(OrbitWorldTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitWorldTests)

add_test(
    NAME Orbit.WorldCoordinates
    COMMAND OrbitWorldTests
)

add_executable(OrbitJobsTests
    JobSystemTests.cpp
)

target_link_libraries(OrbitJobsTests
    PRIVATE
        Orbit::Jobs
)

target_compile_features(OrbitJobsTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitJobsTests)

add_test(
    NAME Orbit.JobSystem
    COMMAND OrbitJobsTests
)

add_executable(OrbitPlanetTerrainTests
    PlanetTerrainTests.cpp
)

target_link_libraries(OrbitPlanetTerrainTests
    PRIVATE
        Orbit::Terrain
)

target_compile_features(OrbitPlanetTerrainTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitPlanetTerrainTests)

add_test(
    NAME Orbit.PlanetTerrain
    COMMAND OrbitPlanetTerrainTests
)

add_executable(OrbitTerrainViewTests
    TerrainViewTests.cpp
)

target_link_libraries(OrbitTerrainViewTests
    PRIVATE
        Orbit::TerrainView
)

target_compile_features(OrbitTerrainViewTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitTerrainViewTests)

add_test(
    NAME Orbit.TerrainView
    COMMAND OrbitTerrainViewTests
)

add_executable(OrbitTerrainCacheTests
    TerrainCacheTests.cpp
)

target_link_libraries(OrbitTerrainCacheTests
    PRIVATE
        Orbit::TerrainCache
)

target_compile_features(OrbitTerrainCacheTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitTerrainCacheTests)

add_test(
    NAME Orbit.TerrainCache
    COMMAND OrbitTerrainCacheTests
)

add_executable(OrbitShaderCompilerTests
    ShaderCompilerTests.cpp
)

target_link_libraries(OrbitShaderCompilerTests
    PRIVATE
        Orbit::ShaderD3D
)

target_compile_features(OrbitShaderCompilerTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitShaderCompilerTests)

add_test(
    NAME Orbit.ShaderCompiler
    COMMAND OrbitShaderCompilerTests
)


add_executable(OrbitTerrainStreamTests
    TerrainStreamTests.cpp
)

target_link_libraries(OrbitTerrainStreamTests
    PRIVATE
        Orbit::TerrainStream
)

target_compile_features(OrbitTerrainStreamTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitTerrainStreamTests)

add_test(
    NAME Orbit.TerrainStream
    COMMAND OrbitTerrainStreamTests
)


add_executable(OrbitTerrainSampleStreamerTests
    TerrainSampleStreamerTests.cpp
)

target_link_libraries(OrbitTerrainSampleStreamerTests
    PRIVATE
        Orbit::TerrainStream
)

target_compile_features(OrbitTerrainSampleStreamerTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitTerrainSampleStreamerTests)

add_test(
    NAME Orbit.TerrainSampleStreamer
    COMMAND OrbitTerrainSampleStreamerTests
)


add_executable(OrbitCameraTests
    CameraTests.cpp
)

target_link_libraries(OrbitCameraTests
    PRIVATE
        Orbit::Camera
)

target_compile_features(OrbitCameraTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitCameraTests)

add_test(
    NAME Orbit.Camera
    COMMAND OrbitCameraTests
)


add_executable(OrbitTerrainFieldsTests
    TerrainFieldsTests.cpp
)

target_link_libraries(OrbitTerrainFieldsTests
    PRIVATE
        Orbit::Terrain
)

target_compile_features(OrbitTerrainFieldsTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitTerrainFieldsTests)

add_test(
    NAME Orbit.TerrainFields
    COMMAND OrbitTerrainFieldsTests
)


add_executable(OrbitTerrainHydrologyTests
    TerrainHydrologyTests.cpp
)

target_link_libraries(OrbitTerrainHydrologyTests
    PRIVATE
        Orbit::TerrainHydrology
)

target_compile_features(OrbitTerrainHydrologyTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitTerrainHydrologyTests)

add_test(
    NAME Orbit.TerrainHydrology
    COMMAND OrbitTerrainHydrologyTests
)


add_executable(OrbitTerrainErosionTests
    TerrainErosionTests.cpp
)

target_link_libraries(OrbitTerrainErosionTests
    PRIVATE
        Orbit::TerrainErosion
)

target_compile_features(OrbitTerrainErosionTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitTerrainErosionTests)

add_test(
    NAME Orbit.TerrainErosion
    COMMAND OrbitTerrainErosionTests
)


add_executable(OrbitTerrainSedimentTests
    TerrainSedimentTests.cpp
)

target_link_libraries(OrbitTerrainSedimentTests
    PRIVATE
        Orbit::TerrainErosion
)

target_compile_features(OrbitTerrainSedimentTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitTerrainSedimentTests)

add_test(
    NAME Orbit.TerrainSediment
    COMMAND OrbitTerrainSedimentTests
)


add_executable(OrbitHydrologyRefinementTests
    HydrologyRefinementTests.cpp
)

target_link_libraries(OrbitHydrologyRefinementTests
    PRIVATE
        Orbit::TerrainErosion
)

target_compile_features(OrbitHydrologyRefinementTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitHydrologyRefinementTests)

add_test(
    NAME Orbit.HydrologyRefinement
    COMMAND OrbitHydrologyRefinementTests
)

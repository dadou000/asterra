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

# V0.0.4 M31 integration acceptance is compiled continuously but is not
# registered as an executable CTest until the validated M30 hardware capture
# opens the final integration gate.
option(
    ORBIT_ENABLE_V004_M31_GATE
    "Register the V0.0.4 M31 final integration acceptance"
    OFF
)

add_executable(OrbitV004IntegrationGateTests
    V004IntegrationGateTests.cpp
)

target_link_libraries(OrbitV004IntegrationGateTests
    PRIVATE
        Orbit::Commands
        Orbit::Documents
        Orbit::EditorModel
        Orbit::Jobs
        Orbit::Scene
        Orbit::Schema
        Orbit::Selection
        Orbit::SurfaceAuthoring
        Orbit::SurfaceModel
        Orbit::Terrain
        Orbit::TerrainBiome
        Orbit::TerrainDebug
        Orbit::TerrainDependency
        Orbit::TerrainErosion
        Orbit::TerrainGeology
        Orbit::TerrainGpu
        Orbit::TerrainHydrology
        Orbit::TerrainMacroGeology
        Orbit::TerrainMaterialColumn
        Orbit::TerrainRender
        Orbit::TerrainScatter
        Orbit::WorldModel
        SQLiteCpp
)

target_compile_features(
    OrbitV004IntegrationGateTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitV004IntegrationGateTests)

if(ORBIT_ENABLE_V004_M31_GATE)
    add_test(
        NAME Orbit.V004IntegrationGate
        COMMAND OrbitV004IntegrationGateTests
    )
endif()

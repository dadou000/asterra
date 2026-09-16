add_library(OrbitProceduralGraph STATIC
    src/ProceduralGraph.cpp
)

add_library(Orbit::ProceduralGraph ALIAS OrbitProceduralGraph)

target_include_directories(OrbitProceduralGraph
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitProceduralGraph
    PUBLIC
        Orbit::Core
        Orbit::Jobs
)

target_compile_features(OrbitProceduralGraph PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitProceduralGraph)

add_library(OrbitRenderGraph STATIC
    src/RenderGraph.cpp
)

add_library(Orbit::RenderGraph ALIAS OrbitRenderGraph)

target_include_directories(OrbitRenderGraph
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitRenderGraph
    PUBLIC
        Orbit::Core
        Orbit::RHI
)

target_compile_features(OrbitRenderGraph PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitRenderGraph)

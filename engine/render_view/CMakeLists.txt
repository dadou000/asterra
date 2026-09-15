add_library(OrbitRenderView STATIC
    src/RenderView.cpp
)

add_library(Orbit::RenderView ALIAS OrbitRenderView)

target_include_directories(OrbitRenderView
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitRenderView
    PUBLIC
        Orbit::Core
        Orbit::Frames
        Orbit::Math
        Orbit::RHI
        Orbit::RenderGraph
        Orbit::Shader
)

target_compile_features(OrbitRenderView PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitRenderView)

add_library(OrbitDebugRender STATIC
    src/VersionOverlayRenderer.cpp
)

add_library(Orbit::DebugRender ALIAS OrbitDebugRender)

target_include_directories(OrbitDebugRender
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitDebugRender
    PUBLIC
        Orbit::Core
        Orbit::RHI
        Orbit::Shader
)

target_compile_features(OrbitDebugRender PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitDebugRender)

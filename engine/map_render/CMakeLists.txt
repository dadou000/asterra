add_library(OrbitMapRender STATIC
    src/PlanetMapRenderer.cpp
)

add_library(Orbit::MapRender ALIAS OrbitMapRender)

target_include_directories(OrbitMapRender
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitMapRender
    PUBLIC
        Orbit::Core
        Orbit::Math
        Orbit::RHI
        Orbit::Shader
        Orbit::Terrain
)

target_compile_features(OrbitMapRender PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitMapRender)

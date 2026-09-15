add_library(OrbitSurface STATIC
    src/SurfaceRegistry.cpp
)

add_library(Orbit::Surface ALIAS OrbitSurface)

target_include_directories(OrbitSurface
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitSurface
    PUBLIC
        Orbit::Core
        Orbit::Math
        Orbit::Terrain
        Orbit::Universe
        Orbit::World
)

target_compile_features(OrbitSurface PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitSurface)

add_library(OrbitUniverse STATIC
    src/BodyRegistry.cpp
    src/ReferenceSurface.cpp
)

add_library(Orbit::Universe ALIAS OrbitUniverse)

target_include_directories(OrbitUniverse
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitUniverse
    PUBLIC
        Orbit::Core
        Orbit::Frames
        Orbit::Math
        Orbit::Time
)

target_compile_features(OrbitUniverse PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitUniverse)

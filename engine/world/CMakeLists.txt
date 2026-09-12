add_library(OrbitWorld STATIC
    src/Planet.cpp
)

add_library(Orbit::World ALIAS OrbitWorld)

target_include_directories(OrbitWorld
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitWorld
    PUBLIC
        Orbit::Core
        Orbit::Math
)

target_compile_features(OrbitWorld PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitWorld)

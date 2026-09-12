add_library(OrbitWorld INTERFACE)

add_library(Orbit::World ALIAS OrbitWorld)

target_include_directories(OrbitWorld
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitWorld
    INTERFACE
        Orbit::Core
        Orbit::Math
)

target_compile_features(OrbitWorld INTERFACE cxx_std_23)

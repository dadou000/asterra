add_library(OrbitSelection STATIC
    src/SelectionService.cpp
)

add_library(Orbit::Selection ALIAS OrbitSelection)

target_include_directories(OrbitSelection
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitSelection
    PUBLIC
        Orbit::Core
        Orbit::Scene
)

target_compile_features(OrbitSelection PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitSelection)

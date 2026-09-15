add_library(OrbitCommands STATIC
    src/CommandRegistry.cpp
    src/CommandService.cpp
)

add_library(Orbit::Commands ALIAS OrbitCommands)

target_include_directories(OrbitCommands
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitCommands
    PUBLIC
        Orbit::Core
        Orbit::Scene
        Orbit::Schema
)

target_compile_features(OrbitCommands PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitCommands)

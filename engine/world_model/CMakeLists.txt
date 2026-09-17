add_library(OrbitWorldModel STATIC
    src/WorldSchemas.cpp
    src/UniverseComposition.cpp
)

add_library(Orbit::WorldModel ALIAS OrbitWorldModel)

target_include_directories(OrbitWorldModel
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitWorldModel
    PUBLIC
        Orbit::Core
        Orbit::Frames
        Orbit::Math
        Orbit::Scene
        Orbit::Schema
        Orbit::Time
        Orbit::Universe
)

target_compile_features(OrbitWorldModel PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitWorldModel)

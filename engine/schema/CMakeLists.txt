add_library(OrbitSchema STATIC
    src/SchemaRegistry.cpp
)

add_library(Orbit::Schema ALIAS OrbitSchema)

target_include_directories(OrbitSchema
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitSchema
    PUBLIC
        Orbit::Core
        Orbit::Math
)

target_compile_features(OrbitSchema PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitSchema)

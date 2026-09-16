add_library(OrbitFields STATIC
    src/FieldRegistry.cpp
)

add_library(Orbit::Fields ALIAS OrbitFields)

target_include_directories(OrbitFields
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitFields
    PUBLIC
        Orbit::Core
        Orbit::Frames
        Orbit::Math
        Orbit::Universe
)

target_compile_features(OrbitFields PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitFields)

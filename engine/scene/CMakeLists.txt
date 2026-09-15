add_library(OrbitScene STATIC
    src/ObjectStore.cpp
)

add_library(Orbit::Scene ALIAS OrbitScene)

target_include_directories(OrbitScene
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitScene
    PUBLIC
        Orbit::Core
        Orbit::Documents
        Orbit::Schema
    PRIVATE
        SQLiteCpp
)

target_compile_features(OrbitScene PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitScene)

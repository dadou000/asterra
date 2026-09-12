add_library(OrbitCamera STATIC
    src/FreeCamera.cpp
)

add_library(Orbit::Camera ALIAS OrbitCamera)

target_include_directories(OrbitCamera
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitCamera
    PUBLIC
        Orbit::Core
        Orbit::Math
)

target_compile_features(OrbitCamera PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitCamera)

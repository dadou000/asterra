add_library(OrbitCore STATIC
    src/Log.cpp
)

add_library(Orbit::Core ALIAS OrbitCore)

target_include_directories(OrbitCore
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(OrbitCore PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitCore)

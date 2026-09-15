add_library(OrbitRuntime STATIC
    src/RuntimeSession.cpp
)

add_library(Orbit::Runtime ALIAS OrbitRuntime)

target_include_directories(OrbitRuntime
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitRuntime
    PUBLIC
        Orbit::Core
        Orbit::Platform
        Orbit::RHI
        Orbit::RhiVulkan
)

target_compile_features(OrbitRuntime PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitRuntime)

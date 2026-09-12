add_library(OrbitPlatform STATIC
    src/windows/Win32Window.cpp
)

add_library(Orbit::Platform ALIAS OrbitPlatform)

target_include_directories(OrbitPlatform
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitPlatform
    PUBLIC
        Orbit::Core
    PRIVATE
        user32
)

target_compile_features(OrbitPlatform PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitPlatform)

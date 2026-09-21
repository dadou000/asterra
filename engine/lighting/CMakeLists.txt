add_library(OrbitLighting STATIC
    src/SurfaceData.cpp
)

add_library(Orbit::Lighting ALIAS OrbitLighting)

target_include_directories(OrbitLighting
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitLighting
    PUBLIC
        Orbit::Core
        Orbit::Math
)

target_compile_features(OrbitLighting PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitLighting)

if(BUILD_TESTING)
    add_executable(OrbitLightingSurfaceDataTests
        tests/SurfaceDataTests.cpp
    )

    add_executable(OrbitLightingSurfaceBufferTests
        tests/SurfaceBufferTests.cpp
    )

    target_link_libraries(OrbitLightingSurfaceDataTests
        PRIVATE
            Orbit::Lighting
    )

    target_compile_features(
        OrbitLightingSurfaceDataTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitLightingSurfaceDataTests)

    target_link_libraries(OrbitLightingSurfaceBufferTests
        PRIVATE
            Orbit::Lighting
    )
    target_compile_features(
        OrbitLightingSurfaceBufferTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitLightingSurfaceBufferTests)

    add_test(
        NAME Orbit.LightingSurfaceData
        COMMAND OrbitLightingSurfaceDataTests
    )

    add_test(
        NAME Orbit.LightingSurfaceBuffer
        COMMAND OrbitLightingSurfaceBufferTests
    )
endif()

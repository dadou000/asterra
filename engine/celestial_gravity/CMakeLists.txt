add_library(OrbitCelestialGravity STATIC
    src/GravityService.cpp
)

add_library(Orbit::CelestialGravity ALIAS OrbitCelestialGravity)

target_include_directories(OrbitCelestialGravity
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitCelestialGravity
    PUBLIC
        Orbit::Core
        Orbit::Frames
        Orbit::Math
        Orbit::Time
)

target_compile_features(OrbitCelestialGravity PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitCelestialGravity)

if(BUILD_TESTING)
    add_executable(OrbitGravityServiceTests
        tests/GravityServiceTests.cpp
    )

    target_link_libraries(OrbitGravityServiceTests
        PRIVATE
            Orbit::CelestialGravity
    )

    target_compile_features(
        OrbitGravityServiceTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitGravityServiceTests)

    add_test(
        NAME Orbit.GravityService
        COMMAND OrbitGravityServiceTests
    )
endif()

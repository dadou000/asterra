add_executable(OrbitWorldTests
    WorldCoordinatesTests.cpp
)

target_link_libraries(OrbitWorldTests
    PRIVATE
        Orbit::World
)

target_compile_features(OrbitWorldTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitWorldTests)

add_test(
    NAME Orbit.WorldCoordinates
    COMMAND OrbitWorldTests
)

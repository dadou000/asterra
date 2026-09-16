add_library(OrbitPathGeometry STATIC
    src/PathDerived.cpp
    src/PathSource.cpp
)

add_library(
    Orbit::PathGeometry
    ALIAS OrbitPathGeometry
)

target_include_directories(
    OrbitPathGeometry
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
    OrbitPathGeometry
    PUBLIC
        Orbit::Core
        Orbit::Frames
        Orbit::Math
        Orbit::PathRouting
        Orbit::Paths
        Orbit::Scene
        Orbit::Time
        Orbit::Universe
)

target_compile_features(
    OrbitPathGeometry
    PUBLIC cxx_std_23
)
orbit_enable_warnings(OrbitPathGeometry)

if(BUILD_TESTING)
    add_executable(
        OrbitPathGeometryTests
        tests/PathDerivedTests.cpp
    )

    target_link_libraries(
        OrbitPathGeometryTests
        PRIVATE
            Orbit::PathGeometry
    )

    target_compile_features(
        OrbitPathGeometryTests
        PRIVATE cxx_std_23
    )
    orbit_enable_warnings(
        OrbitPathGeometryTests
    )

    add_test(
        NAME Orbit.PathGeometry
        COMMAND OrbitPathGeometryTests
    )

    add_executable(
        OrbitPathSourceTests
        tests/PathSourceTests.cpp
    )

    target_link_libraries(
        OrbitPathSourceTests
        PRIVATE
            Orbit::PathGeometry
    )

    target_compile_features(
        OrbitPathSourceTests
        PRIVATE cxx_std_23
    )
    orbit_enable_warnings(
        OrbitPathSourceTests
    )

    add_test(
        NAME Orbit.PathSource
        COMMAND OrbitPathSourceTests
    )
endif()

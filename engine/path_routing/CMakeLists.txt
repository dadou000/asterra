add_library(OrbitPathRouting STATIC
    src/RoutePlanner.cpp
    src/RouteDomains.cpp
)

add_library(Orbit::PathRouting ALIAS OrbitPathRouting)

target_include_directories(OrbitPathRouting
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitPathRouting
    PUBLIC
        Orbit::Core
        Orbit::Fields
        Orbit::Frames
        Orbit::Jobs
        Orbit::Math
        Orbit::Paths
        Orbit::Surface
        Orbit::Terrain
        Orbit::Time
        Orbit::Universe
)

target_compile_features(OrbitPathRouting PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitPathRouting)

if(BUILD_TESTING)
    add_executable(OrbitPathRoutingTests
        tests/RoutePlannerTests.cpp
    )

    target_link_libraries(OrbitPathRoutingTests
        PRIVATE
            Orbit::PathRouting
    )

    target_compile_features(OrbitPathRoutingTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitPathRoutingTests)

    add_test(
        NAME Orbit.PathRouting
        COMMAND OrbitPathRoutingTests
    )

    add_executable(OrbitPathRoutingStaleTests
        tests/RoutePlannerStaleTests.cpp
    )

    target_link_libraries(OrbitPathRoutingStaleTests
        PRIVATE
            Orbit::PathRouting
    )

    target_compile_features(OrbitPathRoutingStaleTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitPathRoutingStaleTests)

    add_test(
        NAME Orbit.PathRoutingStale
        COMMAND OrbitPathRoutingStaleTests
    )
endif()

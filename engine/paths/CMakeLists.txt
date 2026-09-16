add_library(OrbitPaths STATIC
    src/PathNetwork.cpp
)

add_library(Orbit::Paths ALIAS OrbitPaths)

target_include_directories(OrbitPaths
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitPaths
    PUBLIC
        Orbit::Commands
        Orbit::Core
        Orbit::Frames
        Orbit::Math
        Orbit::Scene
        Orbit::Schema
        Orbit::Universe
)

target_compile_features(OrbitPaths PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitPaths)

if(BUILD_TESTING)
    add_executable(OrbitPathNetworkTests
        tests/PathNetworkTests.cpp
    )

    target_link_libraries(OrbitPathNetworkTests
        PRIVATE
            Orbit::Documents
            Orbit::Paths
    )

    target_compile_features(OrbitPathNetworkTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitPathNetworkTests)

    add_test(
        NAME Orbit.PathNetwork
        COMMAND OrbitPathNetworkTests
    )
endif()

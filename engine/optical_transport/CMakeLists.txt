add_library(OrbitOpticalTransport STATIC
    src/Atmosphere.cpp
    src/CacheStamp.cpp
    src/CloudRepresentation.cpp
    src/DischargeGraph.cpp
    src/EmitterTree.cpp
    src/OpticalHierarchy.cpp
    src/StellarVisibility.cpp
)

add_library(
    Orbit::OpticalTransport
    ALIAS OrbitOpticalTransport
)

target_include_directories(
    OrbitOpticalTransport
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(
    OrbitOpticalTransport PUBLIC cxx_std_23)
orbit_enable_warnings(
    OrbitOpticalTransport)

if(BUILD_TESTING)
    add_executable(
        OrbitOpticalTransportTests
        tests/OpticalTransportTests.cpp
    )

    target_link_libraries(
        OrbitOpticalTransportTests
        PRIVATE
            Orbit::OpticalTransport
    )

    target_compile_features(
        OrbitOpticalTransportTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitOpticalTransportTests)

    add_test(
        NAME Orbit.OpticalTransport
        COMMAND OrbitOpticalTransportTests
    )
endif()

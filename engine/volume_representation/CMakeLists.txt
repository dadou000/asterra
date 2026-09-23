add_library(OrbitVolumeRepresentation STATIC
    src/VolumeRepresentation.cpp
    src/VolumeCache.cpp
    src/VolumeOutputCoupling.cpp
    src/VolumeOutputDispatch.cpp
)

add_library(Orbit::VolumeRepresentation ALIAS OrbitVolumeRepresentation)

target_include_directories(OrbitVolumeRepresentation
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitVolumeRepresentation
    PUBLIC
        Orbit::Core
        Orbit::Math
        Orbit::Scene
        Orbit::WorldModel
)

target_compile_features(OrbitVolumeRepresentation PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitVolumeRepresentation)

if(BUILD_TESTING)
    add_executable(OrbitVolumeRepresentationTests
        tests/VolumeRepresentationTests.cpp
        tests/VolumeCacheTests.cpp
        tests/VolumeOutputCouplingTests.cpp
    )

    target_link_libraries(OrbitVolumeRepresentationTests
        PRIVATE Orbit::VolumeRepresentation)
    target_compile_features(OrbitVolumeRepresentationTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitVolumeRepresentationTests)

    add_test(
        NAME Orbit.VolumeRepresentation
        COMMAND OrbitVolumeRepresentationTests
    )
endif()

add_library(OrbitVolumeFields STATIC
    src/VolumeFieldStorage.cpp
    src/VolumeFieldAllocationPolicy.cpp
)

add_library(Orbit::VolumeFields ALIAS OrbitVolumeFields)

target_include_directories(OrbitVolumeFields
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitVolumeFields
    PUBLIC
        Orbit::Core
        Orbit::Math
        Orbit::RenderGraph
        Orbit::RHI
        Orbit::Scene
        Orbit::VolumeRepresentation
        Orbit::WorldModel
)

target_compile_features(OrbitVolumeFields PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitVolumeFields)

if(BUILD_TESTING)
    add_executable(OrbitVolumeFieldStorageTests
        tests/VolumeFieldStorageTests.cpp
    )

    target_link_libraries(OrbitVolumeFieldStorageTests
        PRIVATE
            Orbit::VolumeFields
    )

    target_compile_features(
        OrbitVolumeFieldStorageTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitVolumeFieldStorageTests)

    add_test(
        NAME Orbit.VolumeFieldStorage
        COMMAND OrbitVolumeFieldStorageTests
    )
endif()

add_library(OrbitVolumeLod STATIC
    src/VolumeRepresentationPolicy.cpp
)

add_library(Orbit::VolumeLod ALIAS OrbitVolumeLod)

target_include_directories(OrbitVolumeLod
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitVolumeLod
    PUBLIC
        Orbit::Core
        Orbit::Frames
        Orbit::Math
        Orbit::Scene
        Orbit::Universe
        Orbit::WorldModel
)

target_compile_features(OrbitVolumeLod PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitVolumeLod)

if(BUILD_TESTING)
    add_executable(OrbitVolumeRepresentationPolicyTests
        tests/VolumeRepresentationPolicyTests.cpp
    )

    target_link_libraries(OrbitVolumeRepresentationPolicyTests
        PRIVATE
            Orbit::VolumeLod
    )

    target_compile_features(
        OrbitVolumeRepresentationPolicyTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitVolumeRepresentationPolicyTests)

    add_test(
        NAME Orbit.VolumeRepresentationPolicy
        COMMAND OrbitVolumeRepresentationPolicyTests
    )
endif()

add_library(OrbitCelestialClouds STATIC
    src/CloudField.cpp
    src/CloudRenderer.cpp
    src/WeatherModel.cpp
)

add_library(
    Orbit::CelestialClouds
    ALIAS OrbitCelestialClouds
)

target_include_directories(
    OrbitCelestialClouds
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
    OrbitCelestialClouds
    PUBLIC
        Orbit::CelestialAppearance
        Orbit::CelestialAtmosphere
        Orbit::Core
        Orbit::Math
        Orbit::RHI
        Orbit::Shader
        Orbit::Terrain
        Orbit::Time
)

target_compile_features(
    OrbitCelestialClouds PUBLIC cxx_std_23)
orbit_enable_warnings(
    OrbitCelestialClouds)

if(BUILD_TESTING)
    add_executable(
        OrbitCelestialCloudTests
        tests/CloudFieldTests.cpp
    )

    target_link_libraries(
        OrbitCelestialCloudTests
        PRIVATE
            Orbit::CelestialClouds
    )

    target_compile_features(
        OrbitCelestialCloudTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitCelestialCloudTests)

    add_executable(
        OrbitWeatherMapDump
        tests/WeatherMapDump.cpp
    )
    target_link_libraries(
        OrbitWeatherMapDump
        PRIVATE
            Orbit::CelestialClouds
    )
    target_compile_features(
        OrbitWeatherMapDump PRIVATE cxx_std_23)

    add_test(
        NAME Orbit.CelestialClouds
        COMMAND OrbitCelestialCloudTests
    )

    # Real Vulkan execution test (hence the gpu label, which CI builds but does not run): proves
    # the light-volume refresh skip is bit-identical to refreshing every frame.
    add_executable(
        OrbitCloudLightVolumeTests
        tests/CloudLightVolumeTests.cpp
    )
    target_link_libraries(
        OrbitCloudLightVolumeTests
        PRIVATE
            Orbit::CelestialClouds
            Orbit::RhiVulkan
            Orbit::ShaderDxc
    )
    target_compile_features(
        OrbitCloudLightVolumeTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitCloudLightVolumeTests)
    add_custom_command(TARGET OrbitCloudLightVolumeTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ORBIT_DXCOMPILER_DLL}"
            "$<TARGET_FILE_DIR:OrbitCloudLightVolumeTests>"
    )
    add_test(
        NAME Orbit.CloudLightVolume
        COMMAND OrbitCloudLightVolumeTests
    )
    set_tests_properties(Orbit.CloudLightVolume PROPERTIES LABELS "gpu")
endif()

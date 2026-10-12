add_library(OrbitCelestialClouds STATIC
    src/CloudFarField.cpp
    src/CloudField.cpp
    src/CloudRenderer.cpp
    src/StormSchedule.cpp
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
        Orbit::OpticalTransport
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

    add_executable(
        OrbitSparseStormZonesTests
        tests/SparseStormZonesTests.cpp
    )
    target_link_libraries(
        OrbitSparseStormZonesTests
        PRIVATE Orbit::CelestialClouds
    )
    target_compile_features(OrbitSparseStormZonesTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitSparseStormZonesTests)
    add_test(
        NAME Orbit.SparseStormZones
        COMMAND OrbitSparseStormZonesTests
    )

    add_executable(OrbitStormZoneRuntimeTests tests/StormZoneRuntimeTests.cpp)
    target_link_libraries(OrbitStormZoneRuntimeTests PRIVATE Orbit::CelestialClouds)
    target_compile_features(OrbitStormZoneRuntimeTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitStormZoneRuntimeTests)
    add_test(NAME Orbit.StormZoneRuntime COMMAND OrbitStormZoneRuntimeTests)

    add_executable(OrbitWeatherMultirateSchedulerTests
        tests/WeatherMultirateSchedulerTests.cpp)
    target_link_libraries(OrbitWeatherMultirateSchedulerTests PRIVATE Orbit::CelestialClouds)
    target_compile_features(OrbitWeatherMultirateSchedulerTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitWeatherMultirateSchedulerTests)
    add_test(NAME Orbit.WeatherMultirateScheduler COMMAND OrbitWeatherMultirateSchedulerTests)

    add_executable(OrbitWeatherEvolutionTests tests/WeatherEvolutionTests.cpp)
    target_link_libraries(OrbitWeatherEvolutionTests PRIVATE Orbit::CelestialClouds)
    target_compile_features(OrbitWeatherEvolutionTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitWeatherEvolutionTests)
    add_test(NAME Orbit.WeatherEvolution COMMAND OrbitWeatherEvolutionTests)

    add_executable(
        OrbitStormScheduleTests
        tests/StormScheduleTests.cpp
    )
    target_link_libraries(
        OrbitStormScheduleTests
        PRIVATE
            Orbit::CelestialClouds
    )
    target_compile_features(
        OrbitStormScheduleTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitStormScheduleTests)
    add_test(
        NAME Orbit.StormSchedule
        COMMAND OrbitStormScheduleTests
    )

    add_test(
        NAME Orbit.CelestialClouds
        COMMAND OrbitCelestialCloudTests
    )

    add_executable(
        OrbitCelestialWeatherTimewarpTests
        tests/WeatherTimewarpTests.cpp
    )
    target_link_libraries(
        OrbitCelestialWeatherTimewarpTests
        PRIVATE
            Orbit::CelestialClouds
    )
    target_compile_features(
        OrbitCelestialWeatherTimewarpTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitCelestialWeatherTimewarpTests)
    add_test(
        NAME Orbit.CelestialWeatherTimewarp
        COMMAND OrbitCelestialWeatherTimewarpTests
    )

    add_executable(
        OrbitCelestialCloudFarFieldTests
        tests/CloudFarFieldTests.cpp
    )
    target_link_libraries(
        OrbitCelestialCloudFarFieldTests
        PRIVATE
            Orbit::CelestialClouds
    )
    target_compile_features(
        OrbitCelestialCloudFarFieldTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitCelestialCloudFarFieldTests)
    add_test(
        NAME Orbit.CelestialCloudFarField
        COMMAND OrbitCelestialCloudFarFieldTests
    )
endif()

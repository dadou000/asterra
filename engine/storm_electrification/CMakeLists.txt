add_library(OrbitStormElectrification STATIC
    src/ChargeGrid.cpp
    src/Electrification.cpp
    src/FlashSchedule.cpp
    src/LeaderSimulation.cpp
    src/WarmRainAdapter.cpp
    src/StormLightningSource.cpp
)

add_library(
    Orbit::StormElectrification
    ALIAS OrbitStormElectrification
)

target_include_directories(
    OrbitStormElectrification
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
    OrbitStormElectrification
    PUBLIC
        Orbit::OpticalTransport
)

target_compile_features(
    OrbitStormElectrification PUBLIC cxx_std_23)
orbit_enable_warnings(
    OrbitStormElectrification)

if(BUILD_TESTING)
    add_executable(
        OrbitStormElectrificationTests
        tests/StormElectrificationTests.cpp
    )

    target_link_libraries(
        OrbitStormElectrificationTests
        PRIVATE
            Orbit::StormElectrification
    )

    target_compile_features(
        OrbitStormElectrificationTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitStormElectrificationTests)

    add_test(
        NAME Orbit.StormElectrification
        COMMAND OrbitStormElectrificationTests
    )

    add_executable(
        OrbitStormToLightsTests
        tests/StormToLightsTests.cpp
    )

    target_link_libraries(
        OrbitStormToLightsTests
        PRIVATE
            Orbit::StormElectrification
            Orbit::Lighting
    )

    target_compile_features(
        OrbitStormToLightsTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitStormToLightsTests)

    add_test(
        NAME Orbit.StormToLights
        COMMAND OrbitStormToLightsTests
    )

    add_executable(
        OrbitWarmRainAdapterTests
        tests/WarmRainAdapterTests.cpp
    )

    target_link_libraries(
        OrbitWarmRainAdapterTests
        PRIVATE
            Orbit::StormElectrification
            Orbit::WeatherLab
    )

    target_compile_features(
        OrbitWarmRainAdapterTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitWarmRainAdapterTests)

    add_test(
        NAME Orbit.WarmRainAdapter
        COMMAND OrbitWarmRainAdapterTests
    )

    add_executable(
        OrbitStormLightningSourceTests
        tests/StormLightningSourceTests.cpp
    )

    target_link_libraries(
        OrbitStormLightningSourceTests
        PRIVATE
            Orbit::StormElectrification
    )

    target_compile_features(
        OrbitStormLightningSourceTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitStormLightningSourceTests)

    add_test(
        NAME Orbit.StormLightningSource
        COMMAND OrbitStormLightningSourceTests
    )
endif()

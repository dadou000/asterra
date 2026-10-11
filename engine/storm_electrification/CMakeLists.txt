add_library(OrbitStormElectrification STATIC
    src/ChargeGrid.cpp
    src/Electrification.cpp
    src/FlashSchedule.cpp
    src/LeaderSimulation.cpp
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
endif()

add_library(OrbitCelestialScheduler STATIC
    src/CelestialWorkScheduler.cpp
)
add_library(
    Orbit::CelestialScheduler
    ALIAS OrbitCelestialScheduler)

target_include_directories(
    OrbitCelestialScheduler
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(
    OrbitCelestialScheduler
    PUBLIC
        Orbit::Core)

target_compile_features(
    OrbitCelestialScheduler PUBLIC cxx_std_23)
orbit_enable_warnings(
    OrbitCelestialScheduler)

if(BUILD_TESTING)
    add_executable(
        OrbitCelestialSchedulerTests
        tests/CelestialWorkSchedulerTests.cpp)
    target_link_libraries(
        OrbitCelestialSchedulerTests PRIVATE
        Orbit::CelestialScheduler)
    target_compile_features(
        OrbitCelestialSchedulerTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitCelestialSchedulerTests)
    add_test(
        NAME Orbit.CelestialScheduler
        COMMAND OrbitCelestialSchedulerTests)
endif()

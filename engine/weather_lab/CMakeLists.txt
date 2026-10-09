# SC-01 weather lab: standalone atmosphere experiments (CM1 comparison data
# format, fast storm solver, storm metrics). Pure C++ with no engine
# dependencies so it builds and tests on any host.
find_package(Threads REQUIRED)

add_library(OrbitWeatherLab STATIC
    src/Thermo.cpp
    src/WxFormat.cpp
    src/FastStormSolver.cpp
    src/StormMetrics.cpp
    src/WeatherLabSession.cpp
)

add_library(Orbit::WeatherLab ALIAS OrbitWeatherLab)

target_include_directories(OrbitWeatherLab
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitWeatherLab PUBLIC Threads::Threads)

target_compile_features(OrbitWeatherLab PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitWeatherLab)

add_executable(orbit_weather_lab tools/WeatherLabCli.cpp)
target_link_libraries(orbit_weather_lab PRIVATE Orbit::WeatherLab)
target_compile_features(orbit_weather_lab PRIVATE cxx_std_23)
orbit_enable_warnings(orbit_weather_lab)

if(BUILD_TESTING)
    add_executable(OrbitWeatherLabTests tests/WeatherLabTests.cpp)
    target_link_libraries(OrbitWeatherLabTests PRIVATE Orbit::WeatherLab)
    target_compile_features(OrbitWeatherLabTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitWeatherLabTests)

    add_test(NAME Orbit.WeatherLab COMMAND OrbitWeatherLabTests)
endif()

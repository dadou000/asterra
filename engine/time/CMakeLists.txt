add_library(OrbitTime STATIC
    src/SimulationClock.cpp
)

add_library(Orbit::Time ALIAS OrbitTime)

target_include_directories(OrbitTime
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitTime
    PUBLIC
        Orbit::Core
)

target_compile_features(OrbitTime PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitTime)

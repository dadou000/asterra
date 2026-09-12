add_library(OrbitJobs STATIC
    src/JobSystem.cpp
)

add_library(Orbit::Jobs ALIAS OrbitJobs)

target_include_directories(OrbitJobs
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitJobs
    PUBLIC
        Orbit::Core
)

target_compile_features(OrbitJobs PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitJobs)

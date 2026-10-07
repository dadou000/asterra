add_library(OrbitTime INTERFACE)

add_library(Orbit::Time ALIAS OrbitTime)

target_include_directories(OrbitTime
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitTime
    INTERFACE
        Orbit::Core
)

target_compile_features(OrbitTime INTERFACE cxx_std_23)

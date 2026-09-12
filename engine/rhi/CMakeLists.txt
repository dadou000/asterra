add_library(OrbitRHI INTERFACE)

add_library(Orbit::RHI ALIAS OrbitRHI)

target_include_directories(OrbitRHI
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitRHI
    INTERFACE
        Orbit::Core
)

target_compile_features(OrbitRHI INTERFACE cxx_std_23)

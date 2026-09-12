add_library(OrbitMath INTERFACE)

add_library(Orbit::Math ALIAS OrbitMath)

target_include_directories(OrbitMath
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitMath
    INTERFACE
        Orbit::Core
)

target_compile_features(OrbitMath INTERFACE cxx_std_23)

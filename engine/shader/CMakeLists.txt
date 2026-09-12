add_library(OrbitShader INTERFACE)

add_library(Orbit::Shader ALIAS OrbitShader)

target_include_directories(OrbitShader
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitShader
    INTERFACE
        Orbit::Core
)

target_compile_features(OrbitShader INTERFACE cxx_std_23)

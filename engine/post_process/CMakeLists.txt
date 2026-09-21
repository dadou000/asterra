add_library(OrbitPostProcess STATIC
    src/ColorLut.cpp
)

add_library(Orbit::PostProcess ALIAS OrbitPostProcess)

target_include_directories(OrbitPostProcess
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitPostProcess
    PUBLIC
        Orbit::Core
        Orbit::RHI
        Orbit::Shader
)

target_compile_features(OrbitPostProcess PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitPostProcess)

if(BUILD_TESTING)
    add_executable(OrbitColorLutTests
        tests/ColorLutTests.cpp
    )

    target_link_libraries(OrbitColorLutTests
        PRIVATE
            Orbit::PostProcess
    )

    target_compile_features(OrbitColorLutTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitColorLutTests)

    add_test(
        NAME Orbit.ColorLut
        COMMAND OrbitColorLutTests
    )
endif()

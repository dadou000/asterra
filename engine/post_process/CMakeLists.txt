add_library(OrbitPostProcess STATIC
    src/ColorLut.cpp
    src/DisplayResolve.cpp
    src/HumanEyeAdaptation.cpp
    src/HighlightEffects.cpp
    src/LuminanceHistogram.cpp
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

    add_executable(OrbitLuminanceHistogramTests
        tests/LuminanceHistogramTests.cpp
    )

    add_executable(OrbitHumanEyeAdaptationTests
        tests/HumanEyeAdaptationTests.cpp
    )

    add_executable(OrbitHighlightEffectsTests
        tests/HighlightEffectsTests.cpp
    )

    target_link_libraries(OrbitColorLutTests
        PRIVATE
            Orbit::PostProcess
    )

    target_link_libraries(OrbitLuminanceHistogramTests
        PRIVATE
            Orbit::PostProcess
    )

    target_link_libraries(OrbitHumanEyeAdaptationTests
        PRIVATE
            Orbit::PostProcess
    )

    target_link_libraries(OrbitHighlightEffectsTests
        PRIVATE
            Orbit::PostProcess
    )
    target_compile_features(
        OrbitLuminanceHistogramTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitLuminanceHistogramTests)

    target_compile_features(
        OrbitHumanEyeAdaptationTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitHumanEyeAdaptationTests)

    target_compile_features(
        OrbitHighlightEffectsTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitHighlightEffectsTests)

    target_compile_features(OrbitColorLutTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitColorLutTests)

    add_test(
        NAME Orbit.ColorLut
        COMMAND OrbitColorLutTests
    )

    add_test(
        NAME Orbit.LuminanceHistogram
        COMMAND OrbitLuminanceHistogramTests
    )

    add_test(
        NAME Orbit.HumanEyeAdaptation
        COMMAND OrbitHumanEyeAdaptationTests
    )

    add_test(
        NAME Orbit.HighlightEffects
        COMMAND OrbitHighlightEffectsTests
    )
endif()

add_library(OrbitRenderView STATIC
    src/RenderView.cpp
    src/Capture.cpp
)

add_library(Orbit::RenderView ALIAS OrbitRenderView)

target_include_directories(OrbitRenderView
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitRenderView
    PUBLIC
        Orbit::Core
        Orbit::Frames
        Orbit::Math
        Orbit::RHI
        Orbit::RenderGraph
        Orbit::Shader
)

target_compile_features(OrbitRenderView PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitRenderView)

if(BUILD_TESTING)
    add_executable(OrbitViewRayTests
        tests/ViewRayTests.cpp
    )

    target_link_libraries(OrbitViewRayTests
        PRIVATE
            Orbit::RenderView
    )

    target_compile_features(OrbitViewRayTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitViewRayTests)

    add_test(
        NAME Orbit.ViewRay
        COMMAND OrbitViewRayTests
    )
endif()

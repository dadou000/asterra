add_library(OrbitRenderView STATIC
    src/RenderView.cpp
    src/Capture.cpp
    src/CapturePng.cpp
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
        Orbit::Lighting
        Orbit::RHI
        Orbit::RenderGraph
        Orbit::Shader
)

if(WIN32)
    # PNG screenshots and report thumbnails go through the Windows imaging codec.
    target_link_libraries(OrbitRenderView
        PRIVATE
            ole32
            windowscodecs
    )
endif()

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

    if(WIN32)
        add_executable(OrbitCapturePngTests
            tests/CapturePngTests.cpp
        )

        target_link_libraries(OrbitCapturePngTests
            PRIVATE
                Orbit::RenderView
        )

        target_compile_features(OrbitCapturePngTests PRIVATE cxx_std_23)
        orbit_enable_warnings(OrbitCapturePngTests)

        add_test(
            NAME Orbit.CapturePng
            COMMAND OrbitCapturePngTests
        )
    endif()
endif()

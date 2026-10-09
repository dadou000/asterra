add_library(OrbitMeshRender STATIC
    src/EmissiveLights.cpp
    src/GlassSurface.cpp
    src/MeshLibrary.cpp
    src/MeshSurface.cpp
    src/MeshShadow.cpp
    src/MeshSdfScene.cpp
)

add_library(Orbit::MeshRender ALIAS OrbitMeshRender)

target_include_directories(OrbitMeshRender
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitMeshRender
    PUBLIC
        Orbit::Core
        Orbit::Lighting
        Orbit::Math
        Orbit::MeshImport
        Orbit::MeshSdf
        Orbit::RHI
        Orbit::Shader
    PRIVATE
        Orbit::ContentWic
)

target_compile_features(OrbitMeshRender PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitMeshRender)

if(BUILD_TESTING)
    add_executable(OrbitMeshRenderTests
        tests/MeshSurfaceTests.cpp
    )

    target_link_libraries(OrbitMeshRenderTests
        PRIVATE
            Orbit::MeshRender
    )

    target_compile_features(OrbitMeshRenderTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitMeshRenderTests)

    add_test(
        NAME Orbit.MeshRender
        COMMAND OrbitMeshRenderTests
    )

    add_executable(OrbitGlassSurfaceTests
        tests/GlassSurfaceTests.cpp
    )

    target_link_libraries(OrbitGlassSurfaceTests
        PRIVATE
            Orbit::MeshRender
    )

    target_compile_features(OrbitGlassSurfaceTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitGlassSurfaceTests)

    add_test(
        NAME Orbit.GlassSurface
        COMMAND OrbitGlassSurfaceTests
    )

    add_executable(OrbitEmissiveLightsTests
        tests/EmissiveLightsTests.cpp
    )

    target_link_libraries(OrbitEmissiveLightsTests
        PRIVATE
            Orbit::MeshRender
    )

    target_compile_features(OrbitEmissiveLightsTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitEmissiveLightsTests)

    add_test(
        NAME Orbit.EmissiveLights
        COMMAND OrbitEmissiveLightsTests
    )
endif()

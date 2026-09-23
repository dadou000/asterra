add_library(OrbitVolumeRender STATIC
    src/UniversalVolumeRenderer.cpp
    src/VolumeParticleGpuBinding.cpp
    src/VolumeParticleGpuState.cpp
    src/VolumeParticleRenderer.cpp
    src/VolumeRepresentationPass.cpp
)

add_library(Orbit::VolumeRender ALIAS OrbitVolumeRender)

target_include_directories(OrbitVolumeRender
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitVolumeRender
    PUBLIC
        Orbit::Core
        Orbit::Lighting
        Orbit::Math
        Orbit::RenderGraph
        Orbit::RenderView
        Orbit::RHI
        Orbit::Scene
        Orbit::Shader
        Orbit::VolumeFields
        Orbit::VolumeRepresentation
        Orbit::WorldModel
)

target_compile_features(OrbitVolumeRender PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitVolumeRender)

if(BUILD_TESTING)
    add_executable(OrbitUniversalVolumeRendererTests
        tests/UniversalVolumeRendererTests.cpp
    )

    target_link_libraries(OrbitUniversalVolumeRendererTests
        PRIVATE
            Orbit::VolumeRender
    )

    target_compile_features(
        OrbitUniversalVolumeRendererTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitUniversalVolumeRendererTests)

    add_test(
        NAME Orbit.UniversalVolumeRenderer
        COMMAND OrbitUniversalVolumeRendererTests
    )
endif()

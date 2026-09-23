add_library(OrbitVolumeSolver STATIC
    src/SurfaceVolumeSolver.cpp
    src/SurfaceVolumeRepresentationGate.cpp
)

add_library(Orbit::VolumeSolver ALIAS OrbitVolumeSolver)

target_include_directories(OrbitVolumeSolver
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitVolumeSolver
    PUBLIC
        Orbit::Core
        Orbit::Math
        Orbit::RenderGraph
        Orbit::RHI
        Orbit::Scene
        Orbit::Shader
        Orbit::VolumeFields
        Orbit::VolumeRepresentation
        Orbit::WorldModel
)

target_compile_features(OrbitVolumeSolver PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitVolumeSolver)

if(BUILD_TESTING)
    add_executable(OrbitSurfaceVolumeSolverTests
        tests/SurfaceVolumeSolverTests.cpp
    )

    target_link_libraries(OrbitSurfaceVolumeSolverTests
        PRIVATE
            Orbit::VolumeSolver
    )

    target_compile_features(
        OrbitSurfaceVolumeSolverTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitSurfaceVolumeSolverTests)

    add_test(
        NAME Orbit.SurfaceVolumeSolver
        COMMAND OrbitSurfaceVolumeSolverTests
    )
endif()

add_library(OrbitRHI INTERFACE)

add_library(Orbit::RHI ALIAS OrbitRHI)

target_include_directories(OrbitRHI
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitRHI
    INTERFACE
        Orbit::Core
        Orbit::Math
)

target_compile_features(OrbitRHI INTERFACE cxx_std_23)

if(BUILD_TESTING)
    add_executable(OrbitRhiDeferredPipelineTests
        tests/DeferredPipelineTests.cpp)
    target_link_libraries(OrbitRhiDeferredPipelineTests PRIVATE Orbit::RHI)
    target_compile_features(OrbitRhiDeferredPipelineTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitRhiDeferredPipelineTests)
    add_test(NAME Orbit.RhiDeferredPipeline COMMAND OrbitRhiDeferredPipelineTests)
endif()

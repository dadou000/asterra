add_library(OrbitRuntimeProject STATIC
    src/CookedProject.cpp
)

add_library(Orbit::RuntimeProject ALIAS OrbitRuntimeProject)

target_include_directories(OrbitRuntimeProject
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitRuntimeProject
    PUBLIC
        Orbit::Core
        Orbit::Documents
    PRIVATE
        Luau.VM
        tomlplusplus::tomlplusplus
)

target_compile_features(OrbitRuntimeProject PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitRuntimeProject)

if(MSVC)
    target_compile_options(OrbitRuntimeProject PRIVATE /wd4100)
endif()

if(BUILD_TESTING)
    add_executable(OrbitRuntimeProjectTests
        tests/CookedProjectTests.cpp
    )

    target_link_libraries(OrbitRuntimeProjectTests
        PRIVATE
            Orbit::Build
            Orbit::Documents
            Orbit::RuntimeProject
            Orbit::Scene
    )

    target_compile_features(OrbitRuntimeProjectTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitRuntimeProjectTests)

    orbit_copy_dxc_runtime(
        OrbitRuntimeProjectTests
    )

    add_test(
        NAME Orbit.RuntimeProject
        COMMAND OrbitRuntimeProjectTests
    )
endif()

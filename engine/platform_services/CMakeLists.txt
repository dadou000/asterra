add_library(OrbitPlatformServices STATIC
    src/PlatformConfig.cpp
    src/PlatformServices.cpp
)

add_library(Orbit::PlatformServices ALIAS OrbitPlatformServices)

target_include_directories(OrbitPlatformServices
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitPlatformServices
    PUBLIC
        Orbit::Core
    PRIVATE
        tomlplusplus::tomlplusplus
)

target_compile_features(OrbitPlatformServices PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitPlatformServices)

add_subdirectory(steam)

if(BUILD_TESTING)
    add_executable(OrbitPlatformServicesTests
        tests/PlatformServicesTests.cpp
    )

    target_link_libraries(OrbitPlatformServicesTests
        PRIVATE
            Orbit::PlatformServices
    )

    target_compile_features(OrbitPlatformServicesTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitPlatformServicesTests)

    add_test(
        NAME Orbit.PlatformServices
        COMMAND OrbitPlatformServicesTests
    )
endif()

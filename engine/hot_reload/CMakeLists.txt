add_library(OrbitHotReloadApi INTERFACE)
add_library(Orbit::HotReloadApi ALIAS OrbitHotReloadApi)

target_include_directories(OrbitHotReloadApi
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(OrbitHotReloadApi INTERFACE cxx_std_23)

add_library(OrbitHotReload STATIC
    src/ChangeClassifier.cpp
    src/HotIterationDispatch.cpp
    src/HotIterationService.cpp
    src/HotReloadHost.cpp
)
add_library(Orbit::HotReload ALIAS OrbitHotReload)

target_include_directories(OrbitHotReload
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitHotReload
    PUBLIC
        Orbit::HotReloadApi
    PRIVATE
        Orbit::Core
)

# windows.h exposes min/max macros unless NOMINMAX is defined. The hot-reload
# host uses standard-library min()/max() members for filesystem timestamps, so
# keep those Win32 macros out of this target entirely.
if(MSVC)
    target_compile_definitions(OrbitHotReload PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
endif()

target_compile_features(OrbitHotReload PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitHotReload)

if(BUILD_TESTING)
    add_executable(OrbitHotReloadChangeClassifierTests
        tests/ChangeClassifierTests.cpp
    )
    target_link_libraries(OrbitHotReloadChangeClassifierTests
        PRIVATE
            Orbit::HotReload
    )
    target_compile_features(OrbitHotReloadChangeClassifierTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitHotReloadChangeClassifierTests)
    add_test(
        NAME Orbit.HotReload.ChangeClassifier
        COMMAND OrbitHotReloadChangeClassifierTests
    )
endif()

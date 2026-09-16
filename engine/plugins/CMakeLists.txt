add_library(OrbitPluginManifest STATIC
    src/PluginManifest.cpp
)

add_library(Orbit::PluginManifest ALIAS OrbitPluginManifest)

target_include_directories(OrbitPluginManifest
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitPluginManifest
    PUBLIC
        Orbit::Core
    PRIVATE
        tomlplusplus::tomlplusplus
)

target_compile_features(OrbitPluginManifest PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitPluginManifest)


add_library(OrbitPlugins STATIC
    src/PluginManager.cpp
)

add_library(Orbit::Plugins ALIAS OrbitPlugins)

target_include_directories(OrbitPlugins
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitPlugins
    PUBLIC
        Orbit::Commands
        Orbit::Core
        Orbit::Documents
        Orbit::EditorModel
        Orbit::EditorUi
        Orbit::PluginManifest
        Orbit::Scene
        Orbit::Selection
    PRIVATE
        Luau.Compiler
        Luau.VM
)

target_compile_features(OrbitPlugins PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitPlugins)

if(MSVC)
    # Luau 0.738 exposes a few intentionally unused visitor parameters in
    # public headers. Keep Orbit /WX-clean without weakening warnings for
    # Orbit code itself.
    target_compile_options(OrbitPlugins PRIVATE /wd4100)
endif()

if(BUILD_TESTING)
    add_executable(OrbitPluginTests
        tests/PluginTests.cpp
    )

    target_link_libraries(OrbitPluginTests
        PRIVATE
            Orbit::Plugins
            Orbit::Schema
            SQLiteCpp
    )

    target_compile_features(OrbitPluginTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitPluginTests)

    add_test(
        NAME Orbit.Plugins
        COMMAND OrbitPluginTests
    )
endif()

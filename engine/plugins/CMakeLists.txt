add_library(OrbitPlugins STATIC
    src/PluginManager.cpp
    src/PluginManifest.cpp
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
        Orbit::Scene
        Orbit::Selection
    PRIVATE
        Luau.Compiler
        Luau.VM
        tomlplusplus::tomlplusplus
)

target_compile_features(OrbitPlugins PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitPlugins)

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

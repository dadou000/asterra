add_library(OrbitEditorModel STATIC
    src/ShortcutRegistry.cpp
    src/CommandSurfaces.cpp
    src/AuthoringCommands.cpp
    src/BuiltinSchemas.cpp
    src/ExplorerModel.cpp
    src/InspectorModel.cpp
    src/OutputLog.cpp
)

add_library(Orbit::EditorModel ALIAS OrbitEditorModel)

target_include_directories(OrbitEditorModel
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitEditorModel
    PUBLIC
        Orbit::Commands
        Orbit::Core
        Orbit::Documents
        Orbit::Platform
        Orbit::Scene
        Orbit::Schema
        Orbit::Selection
)

target_compile_features(OrbitEditorModel PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitEditorModel)

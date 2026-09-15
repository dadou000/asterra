add_library(OrbitEditorUi STATIC
    src/BodyPreviewRenderer.cpp
    src/EditorUi.cpp
)

add_library(Orbit::EditorUi ALIAS OrbitEditorUi)

target_include_directories(OrbitEditorUi
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitEditorUi
    PUBLIC
        Orbit::Core
        Orbit::Math
        Orbit::Platform
        Orbit::RHI
        Orbit::Shader
        Orbit::Universe
    PRIVATE
        Orbit::ThirdPartyImGui
)

target_compile_features(OrbitEditorUi PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitEditorUi)

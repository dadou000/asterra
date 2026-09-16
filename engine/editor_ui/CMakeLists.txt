add_library(OrbitEditorUi STATIC
    src/BodyPreviewRenderer.cpp
    src/PathPreviewRenderer.cpp
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
        Orbit::Frames
        Orbit::Math
        Orbit::PathGeometry
        Orbit::Platform
        Orbit::RHI
        Orbit::RenderView
        Orbit::Shader
        Orbit::Universe
    PRIVATE
        Orbit::ThirdPartyImGui
)

target_compile_features(OrbitEditorUi PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitEditorUi)

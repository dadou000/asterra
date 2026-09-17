add_library(OrbitEditorRpc STATIC
    src/EditorRpcService.cpp
    src/ProjectWorldRpc.cpp
)

add_library(Orbit::EditorRpc ALIAS OrbitEditorRpc)

target_include_directories(OrbitEditorRpc
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitEditorRpc
    PUBLIC
        Orbit::Commands
        Orbit::Documents
        Orbit::Paths
        Orbit::Rpc
        Orbit::RenderView
        Orbit::Scene
        Orbit::Schema
        Orbit::Selection
    PRIVATE
        Orbit::EditorModel
)

target_compile_features(OrbitEditorRpc PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitEditorRpc)

if(BUILD_TESTING)
    add_executable(OrbitEditorRpcTests
        tests/EditorRpcServiceTests.cpp
    )

    target_link_libraries(OrbitEditorRpcTests
        PRIVATE
            Orbit::EditorModel
            Orbit::EditorRpc
    )

    target_compile_features(OrbitEditorRpcTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitEditorRpcTests)

    add_test(
        NAME Orbit.EditorRpc
        COMMAND OrbitEditorRpcTests
    )

    add_executable(OrbitProjectWorldRpcTests
        tests/ProjectWorldRpcTests.cpp
    )

    target_link_libraries(OrbitProjectWorldRpcTests
        PRIVATE
            Orbit::EditorModel
            Orbit::EditorRpc
    )

    target_compile_features(OrbitProjectWorldRpcTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitProjectWorldRpcTests)

    add_test(
        NAME Orbit.ProjectWorldRpc
        COMMAND OrbitProjectWorldRpcTests
    )
endif()

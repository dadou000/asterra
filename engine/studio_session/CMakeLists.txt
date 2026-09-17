add_library(OrbitStudioSession STATIC
    src/StudioSession.cpp
    src/StudioWorkspace.cpp
    src/StudioWorkspaceRpcHost.cpp
    src/ViewportTargetRegistry.cpp
    src/ViewportTargetRpc.cpp
)

add_library(Orbit::StudioSession ALIAS OrbitStudioSession)

target_include_directories(OrbitStudioSession
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitStudioSession
    PUBLIC
        Orbit::Documents
        Orbit::EditorRpc
        Orbit::EditorSession
        Orbit::Rpc
)

target_compile_features(OrbitStudioSession PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitStudioSession)

if(BUILD_TESTING)
    add_executable(OrbitStudioSessionTests
        tests/StudioSessionTests.cpp
    )

    target_link_libraries(OrbitStudioSessionTests
        PRIVATE
            Orbit::StudioSession
            Orbit::WorldModel
    )

    target_compile_features(OrbitStudioSessionTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitStudioSessionTests)

    add_test(
        NAME Orbit.StudioSession
        COMMAND OrbitStudioSessionTests
    )

    add_executable(OrbitStudioWorkspaceTests
        tests/StudioWorkspaceTests.cpp
    )

    target_link_libraries(OrbitStudioWorkspaceTests
        PRIVATE
            Orbit::StudioSession
            Orbit::WorldModel
    )

    target_compile_features(OrbitStudioWorkspaceTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitStudioWorkspaceTests)

    add_test(
        NAME Orbit.StudioWorkspace
        COMMAND OrbitStudioWorkspaceTests
    )

    add_executable(OrbitStudioWorkspaceRpcHostTests
        tests/StudioWorkspaceRpcHostTests.cpp
    )

    target_link_libraries(OrbitStudioWorkspaceRpcHostTests
        PRIVATE
            Orbit::StudioSession
            Orbit::WorldModel
    )

    target_compile_features(OrbitStudioWorkspaceRpcHostTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitStudioWorkspaceRpcHostTests)

    add_test(
        NAME Orbit.StudioWorkspaceRpcHost
        COMMAND OrbitStudioWorkspaceRpcHostTests
    )

    add_executable(OrbitViewportTargetRegistryTests
        tests/ViewportTargetRegistryTests.cpp
    )

    target_link_libraries(OrbitViewportTargetRegistryTests
        PRIVATE
            Orbit::StudioSession
            Orbit::WorldModel
    )

    target_compile_features(OrbitViewportTargetRegistryTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitViewportTargetRegistryTests)

    add_test(
        NAME Orbit.ViewportTargetRegistry
        COMMAND OrbitViewportTargetRegistryTests
    )

    add_executable(OrbitViewportTargetRpcTests
        tests/ViewportTargetRpcTests.cpp
    )

    target_link_libraries(OrbitViewportTargetRpcTests
        PRIVATE
            Orbit::StudioSession
            Orbit::WorldModel
    )

    target_compile_features(OrbitViewportTargetRpcTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitViewportTargetRpcTests)

    add_test(
        NAME Orbit.ViewportTargetRpc
        COMMAND OrbitViewportTargetRpcTests
    )
endif()

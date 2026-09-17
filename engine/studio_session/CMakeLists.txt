add_library(OrbitStudioSession STATIC
    src/StudioSession.cpp
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
endif()

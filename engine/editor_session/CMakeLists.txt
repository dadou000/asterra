add_library(OrbitEditorSession STATIC
    src/EditorWorldSession.cpp
    src/WorldDocumentsModel.cpp
)

add_library(Orbit::EditorSession ALIAS OrbitEditorSession)

target_include_directories(OrbitEditorSession
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitEditorSession
    PUBLIC
        Orbit::Commands
        Orbit::Documents
        Orbit::EditorModel
        Orbit::Scene
        Orbit::Schema
        Orbit::Selection
        Orbit::WorldModel
)

target_compile_features(OrbitEditorSession PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitEditorSession)

if(BUILD_TESTING)
    add_executable(OrbitEditorWorldSessionTests
        tests/EditorWorldSessionTests.cpp
    )

    target_link_libraries(OrbitEditorWorldSessionTests
        PRIVATE
            Orbit::EditorSession
    )

    target_compile_features(
        OrbitEditorWorldSessionTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitEditorWorldSessionTests)

    add_test(
        NAME Orbit.EditorWorldSession
        COMMAND OrbitEditorWorldSessionTests
    )

    add_executable(OrbitWorldDocumentsModelTests
        tests/WorldDocumentsModelTests.cpp
    )

    target_link_libraries(OrbitWorldDocumentsModelTests
        PRIVATE
            Orbit::EditorSession
    )

    target_compile_features(
        OrbitWorldDocumentsModelTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitWorldDocumentsModelTests)

    add_test(
        NAME Orbit.WorldDocumentsModel
        COMMAND OrbitWorldDocumentsModelTests
    )
endif()

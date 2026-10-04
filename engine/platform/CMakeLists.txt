add_library(OrbitPlatform STATIC
    src/windows/Win32CrashHandler.cpp
    src/windows/Win32FileBrowser.cpp
    src/windows/Win32FileDialog.cpp
    src/windows/Win32Keyboard.cpp
    src/windows/Win32Paths.cpp
    src/windows/Win32Window.cpp
)

add_library(Orbit::Platform ALIAS OrbitPlatform)

target_include_directories(OrbitPlatform
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitPlatform
    PUBLIC
        Orbit::Core
        Orbit::Math
    PRIVATE
        dbghelp
        user32
        gdi32
        ole32
        shell32
)

target_compile_features(OrbitPlatform PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitPlatform)

if(BUILD_TESTING)
    add_executable(OrbitKeyboardLayoutTests
        tests/KeyboardLayoutTests.cpp
    )

    target_include_directories(OrbitKeyboardLayoutTests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/windows
    )

    target_link_libraries(OrbitKeyboardLayoutTests
        PRIVATE
            Orbit::Platform
            user32
    )

    target_compile_features(OrbitKeyboardLayoutTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitKeyboardLayoutTests)

    add_test(NAME Orbit.KeyboardLayout COMMAND OrbitKeyboardLayoutTests)
endif()

include(FetchContent)

# Persistence dependencies are pinned to released versions. They are consumed
# only through Orbit-owned adapter modules so third-party types do not leak
# into engine interfaces.
set(SQLITECPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SQLITECPP_RUN_CPPCHECK OFF CACHE BOOL "" FORCE)
set(SQLITECPP_INTERNAL_SQLITE ON CACHE BOOL "" FORCE)
set(SQLITECPP_INCLUDE_SCRIPT OFF CACHE BOOL "" FORCE)
set(SQLITECPP_USE_STATIC_RUNTIME OFF CACHE BOOL "" FORCE)
set(SQLITE_OMIT_LOAD_EXTENSION ON CACHE BOOL "" FORCE)

FetchContent_Declare(
    sqlitecpp
    GIT_REPOSITORY https://github.com/SRombauts/SQLiteCpp.git
    GIT_TAG 3.3.3
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(sqlitecpp tomlplusplus)


# Dear ImGui is the long-term native editor widget/docking implementation.
# Orbit owns the public editor UI abstraction and renderer/input adapters;
# plugin/public engine APIs never expose ImGui types.
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9b-docking
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(imgui)

add_library(OrbitThirdPartyImGui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
)

target_include_directories(OrbitThirdPartyImGui
    PUBLIC
        ${imgui_SOURCE_DIR}
)

target_compile_features(OrbitThirdPartyImGui PUBLIC cxx_std_23)

if(MSVC)
    target_compile_options(OrbitThirdPartyImGui PRIVATE /W0)
endif()

add_library(Orbit::ThirdPartyImGui ALIAS OrbitThirdPartyImGui)


# Embedded plugin/runtime scripting. Only the VM/compiler are built; Orbit
# supplies the sandbox/global API and does not expose Luau CLI filesystem
# helpers to project plugins.
set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_WEB OFF CACHE BOOL "" FORCE)
set(LUAU_WERROR OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    luau
    GIT_REPOSITORY https://github.com/luau-lang/luau.git
    GIT_TAG 0.738
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(luau)

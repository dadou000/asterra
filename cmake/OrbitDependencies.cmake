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

# JSON is kept behind Orbit::Rpc. Third-party JSON types never appear in
# public engine interfaces; the library is only the parser/serializer backend.
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(nlohmann_json)


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


# Optional HTML panel host for Studio (Windows only). Only the WebView2 COM
# headers are used at build time; WebView2Loader.dll is loaded dynamically at
# run time from beside the executable, so a missing loader or runtime degrades
# to "open in browser" instead of failing to start. The package is pinned by
# hash (nuget.org, Microsoft.Web.WebView2 1.0.4129.50, about 9 MB, BSD-style
# license; the loader is redistributable).
option(ORBIT_ENABLE_WEBVIEW2 "Host HTML panels in Studio with Microsoft WebView2 (Windows)" ON)
set(ORBIT_WEBVIEW2_LOADER_DLL "" CACHE FILEPATH "WebView2Loader.dll copied beside Studio when available")

if(WIN32 AND ORBIT_ENABLE_WEBVIEW2 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    FetchContent_Declare(
        webview2_sdk
        URL https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.4129.50/microsoft.web.webview2.1.0.4129.50.nupkg
        URL_HASH SHA256=d3934f482d484b89fb4825df720c710664e1143a1e90f7b3a60794ef33f473d2
        DOWNLOAD_NAME microsoft.web.webview2.1.0.4129.50.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(webview2_sdk)

    if(EXISTS "${webview2_sdk_SOURCE_DIR}/build/native/include/WebView2.h")
        add_library(OrbitThirdPartyWebView2 INTERFACE)
        target_include_directories(OrbitThirdPartyWebView2
            SYSTEM INTERFACE "${webview2_sdk_SOURCE_DIR}/build/native/include")
        target_compile_definitions(OrbitThirdPartyWebView2 INTERFACE ORBIT_HAS_WEBVIEW2=1)
        add_library(Orbit::ThirdPartyWebView2 ALIAS OrbitThirdPartyWebView2)
        set(ORBIT_WEBVIEW2_LOADER_DLL
            "${webview2_sdk_SOURCE_DIR}/build/native/x64/WebView2Loader.dll"
            CACHE FILEPATH "WebView2Loader.dll copied beside Studio when available" FORCE)
    endif()
endif()

if(NOT TARGET Orbit::ThirdPartyWebView2)
    add_library(OrbitThirdPartyWebView2 INTERFACE)
    add_library(Orbit::ThirdPartyWebView2 ALIAS OrbitThirdPartyWebView2)
endif()

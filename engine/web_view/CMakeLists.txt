add_library(OrbitWebView STATIC
    src/WebBridge.cpp
    src/WebViewHost.cpp
)

add_library(Orbit::WebView ALIAS OrbitWebView)

target_include_directories(OrbitWebView
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# The WebView2 COM headers are build-time only (see cmake/OrbitDependencies.cmake);
# the loader DLL is resolved at run time, so linking does not depend on it.
target_link_libraries(OrbitWebView
    PUBLIC
        Orbit::Core
        Orbit::Rpc
    PRIVATE
        Orbit::ThirdPartyWebView2
)

target_compile_features(OrbitWebView PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitWebView)

if(BUILD_TESTING)
    add_executable(OrbitWebBridgeTests tests/WebBridgeTests.cpp)
    target_link_libraries(OrbitWebBridgeTests PRIVATE Orbit::WebView)
    target_compile_features(OrbitWebBridgeTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitWebBridgeTests)
    add_test(NAME Orbit.WebBridge COMMAND OrbitWebBridgeTests)
endif()

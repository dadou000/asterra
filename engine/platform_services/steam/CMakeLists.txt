add_library(OrbitPlatformServicesSteam STATIC
    src/SteamProvider.cpp
)

add_library(
    Orbit::PlatformServicesSteam
    ALIAS OrbitPlatformServicesSteam
)

target_include_directories(OrbitPlatformServicesSteam
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitPlatformServicesSteam
    PUBLIC
        Orbit::PlatformServices
)

target_compile_features(OrbitPlatformServicesSteam PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitPlatformServicesSteam)

set(
    ORBIT_STEAMWORKS_SDK_ROOT
    ""
    CACHE PATH
    "Path to the Steamworks SDK root (contains public/steam and redistributable_bin)"
)

if(ORBIT_STEAMWORKS_SDK_ROOT)
    set(
        ORBIT_STEAMWORKS_IMPORT_LIBRARY
        "${ORBIT_STEAMWORKS_SDK_ROOT}/redistributable_bin/win64/steam_api64.lib"
    )
    set(
        ORBIT_STEAMWORKS_RUNTIME
        "${ORBIT_STEAMWORKS_SDK_ROOT}/redistributable_bin/win64/steam_api64.dll"
    )

    if(NOT EXISTS "${ORBIT_STEAMWORKS_SDK_ROOT}/public/steam/steam_api.h")
        message(FATAL_ERROR "ORBIT_STEAMWORKS_SDK_ROOT does not contain public/steam/steam_api.h")
    endif()

    if(NOT EXISTS "${ORBIT_STEAMWORKS_IMPORT_LIBRARY}")
        message(FATAL_ERROR "Steamworks win64 import library was not found")
    endif()

    add_library(OrbitPlatformServicesSteamworks STATIC
        src/SteamworksBridge.cpp
    )

    add_library(
        Orbit::PlatformServicesSteamworks
        ALIAS OrbitPlatformServicesSteamworks
    )

    target_include_directories(OrbitPlatformServicesSteamworks
        PRIVATE
            "${ORBIT_STEAMWORKS_SDK_ROOT}/public"
    )

    target_link_libraries(OrbitPlatformServicesSteamworks
        PUBLIC
            Orbit::PlatformServicesSteam
        PRIVATE
            "${ORBIT_STEAMWORKS_IMPORT_LIBRARY}"
    )

    target_compile_features(OrbitPlatformServicesSteamworks PUBLIC cxx_std_23)
    orbit_enable_warnings(OrbitPlatformServicesSteamworks)

    if(EXISTS "${ORBIT_STEAMWORKS_RUNTIME}")
        add_custom_command(
            TARGET OrbitPlatformServicesSteamworks
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ORBIT_STEAMWORKS_RUNTIME}"
                "$<TARGET_FILE_DIR:OrbitPlatformServicesSteamworks>/steam_api64.dll"
        )
    endif()
endif()

if(BUILD_TESTING)
    add_executable(OrbitPlatformServicesSteamTests
        tests/SteamProviderTests.cpp
    )

    target_link_libraries(OrbitPlatformServicesSteamTests
        PRIVATE
            Orbit::PlatformServicesSteam
    )

    target_compile_features(OrbitPlatformServicesSteamTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitPlatformServicesSteamTests)

    add_test(
        NAME Orbit.PlatformServices.Steam
        COMMAND OrbitPlatformServicesSteamTests
    )
endif()

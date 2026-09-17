add_executable(OrbitPlayer
    src/Main.cpp
)

target_link_libraries(OrbitPlayer
    PRIVATE
        Orbit::Core
        Orbit::Documents
        Orbit::Platform
        Orbit::PlatformServices
        Orbit::PlatformServicesSteam
        Orbit::RHI
        Orbit::Runtime
        Orbit::RuntimeProject
        Orbit::Scene
)

if(TARGET Orbit::PlatformServicesSteamworks)
    target_link_libraries(OrbitPlayer
        PRIVATE
            Orbit::PlatformServicesSteamworks
    )

    target_compile_definitions(OrbitPlayer
        PRIVATE
            ORBIT_HAS_STEAMWORKS=1
    )

    if(ORBIT_STEAMWORKS_SDK_ROOT)
        add_custom_command(
            TARGET OrbitPlayer
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ORBIT_STEAMWORKS_SDK_ROOT}/redistributable_bin/win64/steam_api64.dll"
                "$<TARGET_FILE_DIR:OrbitPlayer>/steam_api64.dll"
        )
    endif()
endif()

target_compile_features(OrbitPlayer PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitPlayer)

set_target_properties(OrbitPlayer PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

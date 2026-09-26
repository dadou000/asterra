if(NOT DEFINED SOURCE OR NOT DEFINED DESTINATION)
    message(FATAL_ERROR "OrbitPublishIfUnlocked requires SOURCE and DESTINATION")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SOURCE}" "${DESTINATION}"
    RESULT_VARIABLE ORBIT_PUBLISH_RESULT
    ERROR_VARIABLE ORBIT_PUBLISH_ERROR
    OUTPUT_QUIET
)

if(NOT ORBIT_PUBLISH_RESULT EQUAL 0)
    # The canonical root Orbit.exe is normally locked while the developer is
    # using it. A hot-iteration build must still succeed so the new Studio can
    # be staged under a generation name and launched after the old process
    # exits. The next ordinary build will publish the root executable again.
    message(STATUS
        "Orbit root publish deferred because the destination is in use: ${DESTINATION}")
endif()

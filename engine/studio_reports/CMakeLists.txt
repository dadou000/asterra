add_library(OrbitStudioReports STATIC
    src/IssueReport.cpp
)

add_library(Orbit::StudioReports ALIAS OrbitStudioReports)

target_include_directories(OrbitStudioReports
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitStudioReports
    PUBLIC
        Orbit::Core
        Orbit::Rpc
)

target_compile_features(OrbitStudioReports PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitStudioReports)

if(BUILD_TESTING)
    add_executable(OrbitStudioReportsTests
        tests/IssueReportTests.cpp
    )

    target_link_libraries(OrbitStudioReportsTests
        PRIVATE
            Orbit::StudioReports
    )

    target_compile_features(OrbitStudioReportsTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitStudioReportsTests)

    add_test(
        NAME Orbit.StudioReports
        COMMAND OrbitStudioReportsTests
    )
endif()

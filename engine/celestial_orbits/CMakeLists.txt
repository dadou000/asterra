add_library(OrbitCelestialOrbits STATIC
    src/OrbitState.cpp
    src/ImportedEphemeris.cpp
    src/NBodyDomain.cpp
)

add_library(Orbit::CelestialOrbits ALIAS OrbitCelestialOrbits)

target_include_directories(OrbitCelestialOrbits
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitCelestialOrbits
    PUBLIC
        Orbit::Core
        Orbit::Math
        Orbit::Time
)

target_compile_features(OrbitCelestialOrbits PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitCelestialOrbits)

if(BUILD_TESTING)
    add_executable(OrbitAnalyticConicTests
        tests/AnalyticConicTests.cpp
    )

    target_link_libraries(OrbitAnalyticConicTests
        PRIVATE
            Orbit::CelestialOrbits
    )

    target_compile_features(
        OrbitAnalyticConicTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitAnalyticConicTests)

    add_test(
        NAME Orbit.AnalyticConics
        COMMAND OrbitAnalyticConicTests
    )

    add_executable(OrbitImportedEphemerisTests
        tests/ImportedEphemerisTests.cpp
    )

    target_link_libraries(OrbitImportedEphemerisTests
        PRIVATE
            Orbit::CelestialOrbits
    )

    target_compile_features(
        OrbitImportedEphemerisTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitImportedEphemerisTests)

    add_test(
        NAME Orbit.ImportedEphemeris
        COMMAND OrbitImportedEphemerisTests
    )

    add_executable(OrbitNBodyDomainTests
        tests/NBodyDomainTests.cpp
    )

    target_link_libraries(OrbitNBodyDomainTests
        PRIVATE
            Orbit::CelestialOrbits
    )

    target_compile_features(
        OrbitNBodyDomainTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitNBodyDomainTests)

    add_test(
        NAME Orbit.NBodyDomain
        COMMAND OrbitNBodyDomainTests
    )
endif()

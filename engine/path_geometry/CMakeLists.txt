add_library(OrbitPathGeometry STATIC
    src/PathDerived.cpp
    src/PathSource.cpp
    src/CorridorSweep.cpp
    src/CorridorRefinement.cpp
    src/CorridorDerived.cpp
    src/CorridorTerrainSource.cpp
    src/CorridorEngineering.cpp
    src/CorridorEarthworks.cpp
    src/CorridorDesignPreview.cpp
    src/TransportSections.cpp
    src/TransportTopology.cpp
    src/RailCantDesign.cpp
    src/InfrastructureSectionBuilder.cpp
    src/TransportJunctions.cpp
    src/RailInterlocking.cpp
    src/JunctionConflicts.cpp
    src/JunctionSignalPhases.cpp
    src/RoadJunctionConstruction.cpp
    src/RoadJunctionPublication.cpp
    src/RoadJunctionContact.cpp
    src/RoadContactScene.cpp
    src/RoadVehicleFixture.cpp
    src/RoadVehicleWorld.cpp
)

add_library(
    Orbit::PathGeometry
    ALIAS OrbitPathGeometry
)

target_include_directories(
    OrbitPathGeometry
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
    OrbitPathGeometry
    PUBLIC
        Orbit::Core
        Orbit::Frames
        Orbit::Math
        Orbit::PathRouting
        Orbit::Paths
        Orbit::Scene
        Orbit::Time
        Orbit::Terrain
        Orbit::Universe
)

target_compile_features(
    OrbitPathGeometry
    PUBLIC cxx_std_23
)
orbit_enable_warnings(OrbitPathGeometry)

if(BUILD_TESTING)
    add_executable(
        OrbitPathGeometryTests
        tests/PathDerivedTests.cpp
    )

    target_link_libraries(
        OrbitPathGeometryTests
        PRIVATE
            Orbit::PathGeometry
    )

    target_compile_features(
        OrbitPathGeometryTests
        PRIVATE cxx_std_23
    )
    orbit_enable_warnings(
        OrbitPathGeometryTests
    )

    add_test(
        NAME Orbit.PathGeometry
        COMMAND OrbitPathGeometryTests
    )

    add_executable(
        OrbitPathSourceTests
        tests/PathSourceTests.cpp
    )

    target_link_libraries(
        OrbitPathSourceTests
        PRIVATE
            Orbit::PathGeometry
    )

    target_compile_features(
        OrbitPathSourceTests
        PRIVATE cxx_std_23
    )
    orbit_enable_warnings(
        OrbitPathSourceTests
    )

    add_test(
        NAME Orbit.PathSource
        COMMAND OrbitPathSourceTests
    )
endif()

if(BUILD_TESTING)
    add_executable(OrbitCorridorSweepTests tests/CorridorSweepTests.cpp)
    target_link_libraries(OrbitCorridorSweepTests PRIVATE Orbit::PathGeometry)
    target_compile_features(OrbitCorridorSweepTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitCorridorSweepTests)
    add_test(NAME Orbit.CorridorSweep COMMAND OrbitCorridorSweepTests)
endif()

if(BUILD_TESTING)
    add_executable(OrbitCorridorTerrainSourceTests
        tests/CorridorTerrainSourceTests.cpp)
    target_link_libraries(OrbitCorridorTerrainSourceTests
        PRIVATE Orbit::PathGeometry)
    target_compile_features(OrbitCorridorTerrainSourceTests
        PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitCorridorTerrainSourceTests)
    add_test(NAME Orbit.CorridorTerrainSource
        COMMAND OrbitCorridorTerrainSourceTests)
endif()

if(BUILD_TESTING)
    add_executable(OrbitCorridorEngineeringTests tests/CorridorEngineeringTests.cpp)
    target_link_libraries(OrbitCorridorEngineeringTests PRIVATE Orbit::PathGeometry)
    target_compile_features(OrbitCorridorEngineeringTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitCorridorEngineeringTests)
    add_test(NAME Orbit.CorridorEngineering COMMAND OrbitCorridorEngineeringTests)
endif()

if(BUILD_TESTING)
    add_executable(OrbitCorridorEarthworksTests tests/CorridorEarthworksTests.cpp)
    target_link_libraries(OrbitCorridorEarthworksTests PRIVATE Orbit::PathGeometry)
    target_compile_features(OrbitCorridorEarthworksTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitCorridorEarthworksTests)
    add_test(NAME Orbit.CorridorEarthworks COMMAND OrbitCorridorEarthworksTests)
endif()

if(BUILD_TESTING)
    add_executable(OrbitCorridorDesignPreviewTests tests/CorridorDesignPreviewTests.cpp)
    target_link_libraries(OrbitCorridorDesignPreviewTests PRIVATE Orbit::PathGeometry)
    target_compile_features(OrbitCorridorDesignPreviewTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitCorridorDesignPreviewTests)
    add_test(NAME Orbit.CorridorDesignPreview COMMAND OrbitCorridorDesignPreviewTests)
endif()

if(BUILD_TESTING)
    add_executable(OrbitTransportSectionsTests tests/TransportSectionsTests.cpp)
    target_link_libraries(OrbitTransportSectionsTests PRIVATE Orbit::PathGeometry)
    target_compile_features(OrbitTransportSectionsTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTransportSectionsTests)
    add_test(NAME Orbit.TransportSections COMMAND OrbitTransportSectionsTests)

    add_executable(OrbitTransportTopologyTests tests/TransportTopologyTests.cpp)
    target_link_libraries(OrbitTransportTopologyTests PRIVATE Orbit::PathGeometry)
    target_compile_features(OrbitTransportTopologyTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTransportTopologyTests)
    add_test(NAME Orbit.TransportTopology COMMAND OrbitTransportTopologyTests)
endif()

if(BUILD_TESTING)
    add_executable(OrbitRailCantDesignTests tests/RailCantDesignTests.cpp)
    target_link_libraries(OrbitRailCantDesignTests PRIVATE Orbit::PathGeometry)
    target_compile_features(OrbitRailCantDesignTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitRailCantDesignTests)
    add_test(NAME Orbit.RailCantDesign COMMAND OrbitRailCantDesignTests)
endif()

if(BUILD_TESTING)
    add_executable(OrbitInfrastructureSectionBuilderTests tests/InfrastructureSectionBuilderTests.cpp)
    target_link_libraries(OrbitInfrastructureSectionBuilderTests PRIVATE Orbit::PathGeometry)
    target_compile_features(OrbitInfrastructureSectionBuilderTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitInfrastructureSectionBuilderTests)
    add_test(NAME Orbit.InfrastructureSectionBuilder COMMAND OrbitInfrastructureSectionBuilderTests)
endif()

if(BUILD_TESTING)
    add_executable(OrbitTransportJunctionsTests tests/TransportJunctionsTests.cpp)
    target_link_libraries(OrbitTransportJunctionsTests PRIVATE Orbit::PathGeometry)
    target_compile_features(OrbitTransportJunctionsTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTransportJunctionsTests)
    add_test(NAME Orbit.TransportJunctions COMMAND OrbitTransportJunctionsTests)

    add_executable(OrbitRailInterlockingTests tests/RailInterlockingTests.cpp)
    target_link_libraries(OrbitRailInterlockingTests PRIVATE Orbit::PathGeometry)
    target_compile_features(OrbitRailInterlockingTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitRailInterlockingTests)
    add_test(NAME Orbit.RailInterlocking COMMAND OrbitRailInterlockingTests)
endif()

if(BUILD_TESTING)
    add_executable(OrbitJunctionConflictsTests tests/JunctionConflictsTests.cpp)
    target_link_libraries(OrbitJunctionConflictsTests PRIVATE Orbit::PathGeometry)
    target_compile_features(OrbitJunctionConflictsTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitJunctionConflictsTests)
    add_test(NAME Orbit.JunctionConflicts COMMAND OrbitJunctionConflictsTests)
endif()

if(BUILD_TESTING)
    add_executable(OrbitJunctionSignalPhasesTests tests/JunctionSignalPhasesTests.cpp)
    target_link_libraries(OrbitJunctionSignalPhasesTests PRIVATE Orbit::PathGeometry)
    target_compile_features(OrbitJunctionSignalPhasesTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitJunctionSignalPhasesTests)
    add_test(NAME Orbit.JunctionSignalPhases COMMAND OrbitJunctionSignalPhasesTests)
endif()

if(BUILD_TESTING)
    add_executable(OrbitRoadJunctionConstructionTests
        tests/RoadJunctionConstructionTests.cpp)
    target_link_libraries(OrbitRoadJunctionConstructionTests PRIVATE Orbit::PathGeometry)
    target_compile_features(OrbitRoadJunctionConstructionTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitRoadJunctionConstructionTests)
    add_test(NAME Orbit.RoadJunctionConstruction COMMAND OrbitRoadJunctionConstructionTests)
endif()

if(BUILD_TESTING)
    add_executable(OrbitRoadVehicleWorldTests tests/RoadVehicleWorldTests.cpp)
    target_link_libraries(OrbitRoadVehicleWorldTests PRIVATE Orbit::PathGeometry)
    target_compile_features(OrbitRoadVehicleWorldTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitRoadVehicleWorldTests)
    add_test(NAME Orbit.RoadVehicleWorld COMMAND OrbitRoadVehicleWorldTests)

    add_executable(OrbitRoadVehicleFixtureTests tests/RoadVehicleFixtureTests.cpp)
    target_link_libraries(OrbitRoadVehicleFixtureTests PRIVATE Orbit::PathGeometry)
    target_compile_features(OrbitRoadVehicleFixtureTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitRoadVehicleFixtureTests)
    add_test(NAME Orbit.RoadVehicleFixture COMMAND OrbitRoadVehicleFixtureTests)

    add_executable(OrbitRoadContactSceneTests tests/RoadContactSceneTests.cpp)
    target_link_libraries(OrbitRoadContactSceneTests PRIVATE Orbit::PathGeometry)
    target_compile_features(OrbitRoadContactSceneTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitRoadContactSceneTests)
    add_test(NAME Orbit.RoadContactScene COMMAND OrbitRoadContactSceneTests)

    add_executable(OrbitRoadJunctionContactTests tests/RoadJunctionContactTests.cpp)
    target_link_libraries(OrbitRoadJunctionContactTests PRIVATE Orbit::PathGeometry)
    target_compile_features(OrbitRoadJunctionContactTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitRoadJunctionContactTests)
    add_test(NAME Orbit.RoadJunctionContact COMMAND OrbitRoadJunctionContactTests)

    add_executable(OrbitRoadJunctionPublicationTests tests/RoadJunctionPublicationTests.cpp)
    target_link_libraries(OrbitRoadJunctionPublicationTests PRIVATE Orbit::PathGeometry)
    target_compile_features(OrbitRoadJunctionPublicationTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitRoadJunctionPublicationTests)
    add_test(NAME Orbit.RoadJunctionPublication COMMAND OrbitRoadJunctionPublicationTests)
endif()

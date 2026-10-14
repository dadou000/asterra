#pragma once

#include <orbit/path_geometry/RoadContactScene.hpp>
#include <array>
#include <string>

namespace orbit::path_geometry
{
// First planar/body-frame dynamics fixture. All coordinates are metres in
// ONE static frame with +Y as local up. NOT a general 6-DoF vehicle solver.
struct RoadVehicleParameters
{
    f64 massKg{1500.0};
    f64 wheelbaseMeters{2.7};
    f64 trackMeters{1.55};
    f64 wheelRadiusMeters{0.34};
    f64 suspensionRestMeters{0.46};
    f64 suspensionTravelMeters{0.30};
    f64 springNewtonsPerMeter{33000.0};
    f64 damperNewtonSecondsPerMeter{3500.0};
    f64 pitchInertiaKgM2{2300.0};
    f64 rollInertiaKgM2{1200.0};
    f64 pitchDampingNmSeconds{2400.0};
    f64 rollDampingNmSeconds{1700.0};
    f64 maximumDriveForceNewtons{4200.0};
    f64 maximumBrakeForceNewtons{9000.0};
    f64 tireFrictionCoefficient{0.90};
    f64 aeroDragNewtonsPerMpsSquared{0.38};
    f64 rollingDragNewtonsPerMps{65.0};
    f64 gravityMetersPerSecond2{9.81};
};
struct RoadVehicleInput
{
    f64 throttle{0.0};  // [0,1]
    f64 brake{0.0};     // [0,1]
    f64 steerRadians{0.0}; // [-0.6,0.6]
};
struct RoadVehicleState
{
    math::Double3 centre{};
    f64 forwardSpeedMetersPerSecond{0.0};
    f64 verticalSpeedMetersPerSecond{0.0};
    f64 yawRadians{0.0};
    f64 pitchRadians{0.0};
    f64 rollRadians{0.0};
    f64 pitchRateRadiansPerSecond{0.0};
    f64 rollRateRadiansPerSecond{0.0};
};
struct RoadVehicleStepReport
{
    u32 contactingWheels{0};
    u32 patchWheels{0};
    u32 roadWheels{0};
    f64 suspensionForceNewtons{0.0};
    f64 verticalAccelerationMetersPerSecond2{0.0};
    f64 maximumBottomOutMeters{0.0};
    std::array<scene::ObjectId,4> wheelOwners{};
    bool queryBudgetExceeded{false};
};
struct RoadVehicleFixtureRequest
{
    frames::FrameId frame{};
    RoadVehicleState initial{};
    RoadVehicleParameters vehicle{};
    RoadVehicleInput control{};
    u32 steps{240};
    f64 fixedStepSeconds{1.0/120.0};
    std::size_t maximumTrianglesPerWheel{2048U};
};
struct RoadVehicleFixtureReport
{
    frames::FrameId frame{};
    u64 worldGeneration{0};
    u64 contactPacketSignature{0};
    RoadVehicleState finalState{};
    u32 simulatedSteps{0};
    u32 stepsWithGroundContact{0};
    u32 stepsWithFullContact{0};
    u32 stepsWithPatchContact{0};
    u32 stepsWithRoadContact{0};
    u32 wheelOwnerTransitions{0};
    f64 maximumBottomOutMeters{0.0};
    f64 maximumAbsVerticalAcceleration{0.0};
    f64 maximumAbsPitchRadians{0.0};
    f64 maximumAbsRollRadians{0.0};
    f64 pathDistanceMeters{0.0};
    bool completed{false};
    std::string error;
};

// Fixed timestep independent of viewport FPS or SimulationClock::Rate.
// Each wheel uses exactly the same revision-fenced static contact scene
// consumed by the Studio junction renderer. Failed contact/budget input
// rejects the run; no game scene or vehicle actors are mutated.
[[nodiscard]] RoadVehicleFixtureReport RunRoadVehicleFixture(
    const RoadContactSceneOwner& scene,
    const RoadVehicleFixtureRequest& request);
} // namespace orbit::path_geometry

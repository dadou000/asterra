#include <orbit/path_geometry/RoadVehicleFixture.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace orbit::path_geometry
{
namespace
{
using math::Double3;
bool Finite(f64 x) noexcept {return std::isfinite(x);}
bool Finite(Double3 v) noexcept
{
    return Finite(v.x)&&Finite(v.y)&&Finite(v.z);
}
bool Valid(const RoadVehicleParameters& p) noexcept
{
    return Finite(p.massKg) && p.massKg>100.0 && p.massKg<50000.0 &&
        Finite(p.wheelbaseMeters) && p.wheelbaseMeters>0.5 &&
        p.wheelbaseMeters<20.0 &&
        Finite(p.trackMeters) && p.trackMeters>0.5 && p.trackMeters<10.0 &&
        Finite(p.wheelRadiusMeters) && p.wheelRadiusMeters>0.05 &&
        p.wheelRadiusMeters<3.0 &&
        Finite(p.suspensionRestMeters) && p.suspensionRestMeters>0.01 &&
        p.suspensionRestMeters<3.0 &&
        Finite(p.suspensionTravelMeters) && p.suspensionTravelMeters>0.01 &&
        p.suspensionTravelMeters<=p.suspensionRestMeters &&
        Finite(p.springNewtonsPerMeter) && p.springNewtonsPerMeter>100.0 &&
        p.springNewtonsPerMeter<1000000.0 &&
        Finite(p.damperNewtonSecondsPerMeter) &&
        p.damperNewtonSecondsPerMeter>=0.0 &&
        p.damperNewtonSecondsPerMeter<100000.0 &&
        Finite(p.pitchInertiaKgM2) && p.pitchInertiaKgM2>0.0 &&
        Finite(p.rollInertiaKgM2) && p.rollInertiaKgM2>0.0 &&
        Finite(p.pitchDampingNmSeconds) && p.pitchDampingNmSeconds>=0.0 &&
        Finite(p.rollDampingNmSeconds) && p.rollDampingNmSeconds>=0.0 &&
        Finite(p.maximumDriveForceNewtons) && p.maximumDriveForceNewtons>=0.0 &&
        Finite(p.maximumBrakeForceNewtons) && p.maximumBrakeForceNewtons>=0.0 &&
        Finite(p.tireFrictionCoefficient) &&
        p.tireFrictionCoefficient>=0.0 && p.tireFrictionCoefficient<=3.0 &&
        Finite(p.aeroDragNewtonsPerMpsSquared) &&
        p.aeroDragNewtonsPerMpsSquared>=0.0 &&
        Finite(p.rollingDragNewtonsPerMps) && p.rollingDragNewtonsPerMps>=0.0 &&
        Finite(p.gravityMetersPerSecond2) &&
        p.gravityMetersPerSecond2>0.0 && p.gravityMetersPerSecond2<50.0;
}
bool Valid(const RoadVehicleState& s) noexcept
{
    return Finite(s.centre) &&
        Finite(s.forwardSpeedMetersPerSecond) &&
        Finite(s.verticalSpeedMetersPerSecond) &&
        Finite(s.yawRadians) && Finite(s.pitchRadians) &&
        Finite(s.rollRadians) && Finite(s.pitchRateRadiansPerSecond) &&
        Finite(s.rollRateRadiansPerSecond) &&
        std::abs(s.forwardSpeedMetersPerSecond)<100.0 &&
        std::abs(s.verticalSpeedMetersPerSecond)<100.0 &&
        std::abs(s.pitchRadians)<0.6 && std::abs(s.rollRadians)<0.6;
}
RoadVehicleStepReport Step(const RoadContactSceneOwner& contact,
    const frames::FrameId frame, const RoadVehicleParameters& config,
    const RoadVehicleInput& input, const f64 dt,
    const std::size_t triangleBudget, RoadVehicleState& state)
{
    RoadVehicleStepReport result;
    const f64 a=config.wheelbaseMeters*0.5;
    const f64 b=config.trackMeters*0.5;
    const std::array<f64,4> longitudinal{a,a,-a,-a};
    const std::array<f64,4> lateral{-b,b,-b,b};
    const f64 forwardX=std::cos(state.yawRadians);
    const f64 forwardZ=std::sin(state.yawRadians);
    const Double3 forward{forwardX,0.0,forwardZ};
    const Double3 right{-forwardZ,0.0,forwardX};
    f64 pitchMoment=0.0,rollMoment=0.0;
    // Each wheel is an independent sprung ray. Small-angle body tilt
    // changes the vertical spring mounting point; full rotational tire
    // dynamics are deliberately NOT implied by this CPU fixture.
    for(std::size_t i=0;i<4;++i)
    {
        const auto x=longitudinal[i],z=lateral[i];
        auto hub=state.centre+forward*x+right*z;
        hub.y+=state.pitchRadians*x+state.rollRadians*z;
        const auto hit=contact.Probe({
            .frame=frame,
            .origin=hub,
            .direction={0,-1,0},
            .maximumDistanceMeters=config.wheelRadiusMeters+
                config.suspensionRestMeters+
                config.suspensionTravelMeters+0.3,
            .maximumTriangles=triangleBudget
        });
        if(hit.budgetExceeded)
        {
            result.queryBudgetExceeded=true;
            return result;
        }
        if(!hit.hit || hit.normal.y<=0.25)
            continue;
        const f64 unclampedCompression=
            config.suspensionRestMeters+
            config.wheelRadiusMeters-hit.distanceMeters;
        if(unclampedCompression<=0.0)continue;
        const f64 bottomOut=std::max(0.0,
            unclampedCompression-config.suspensionTravelMeters);
        result.maximumBottomOutMeters=std::max(
            result.maximumBottomOutMeters,bottomOut);
        const f64 compression=std::min(
            unclampedCompression,config.suspensionTravelMeters);
        const f64 springSpeed=state.verticalSpeedMetersPerSecond+
            state.pitchRateRadiansPerSecond*x+
            state.rollRateRadiansPerSecond*z;
        const f64 force=std::clamp(
            config.springNewtonsPerMeter*compression-
            config.damperNewtonSecondsPerMeter*springSpeed,
            0.0,config.springNewtonsPerMeter*
                config.suspensionTravelMeters*1.75);
        result.suspensionForceNewtons+=force;
        pitchMoment+=force*x;
        rollMoment+=force*z;
        ++result.contactingWheels;
        if(hit.junctionPatch)++result.patchWheels;
        else ++result.roadWheels;
        result.wheelOwners[i]=hit.owner;
    }
    result.verticalAccelerationMetersPerSecond2=
        result.suspensionForceNewtons/config.massKg-
        config.gravityMetersPerSecond2;
    state.verticalSpeedMetersPerSecond+=
        result.verticalAccelerationMetersPerSecond2*dt;
    state.centre.y+=state.verticalSpeedMetersPerSecond*dt;
    const f64 pitchAcceleration=(pitchMoment-
        config.pitchDampingNmSeconds*state.pitchRateRadiansPerSecond)/
        config.pitchInertiaKgM2;
    const f64 rollAcceleration=(rollMoment-
        config.rollDampingNmSeconds*state.rollRateRadiansPerSecond)/
        config.rollInertiaKgM2;
    state.pitchRateRadiansPerSecond+=pitchAcceleration*dt;
    state.rollRateRadiansPerSecond+=rollAcceleration*dt;
    state.pitchRadians+=state.pitchRateRadiansPerSecond*dt;
    state.rollRadians+=state.rollRateRadiansPerSecond*dt;

    // Simplified longitudinal tire traction limited by normal load.
    // No wheel slip curves, individual wheel rotation or tire thermal model.
    const f64 tractionLimit=config.tireFrictionCoefficient*
        result.suspensionForceNewtons;
    const f64 requested=input.throttle*config.maximumDriveForceNewtons-
        input.brake*config.maximumBrakeForceNewtons;
    const f64 traction=std::clamp(requested,-tractionLimit,tractionLimit);
    const f64 speed=state.forwardSpeedMetersPerSecond;
    const f64 resist=config.aeroDragNewtonsPerMpsSquared*speed*speed+
        config.rollingDragNewtonsPerMps*speed;
    state.forwardSpeedMetersPerSecond=std::max(0.0,
        speed+(traction-resist)/config.massKg*dt);
    const f64 oldYaw=state.yawRadians;
    state.yawRadians+=(state.forwardSpeedMetersPerSecond/
        config.wheelbaseMeters)*std::tan(input.steerRadians)*dt;
    const f64 heading=(oldYaw+state.yawRadians)*0.5;
    state.centre.x+=std::cos(heading)*
        state.forwardSpeedMetersPerSecond*dt;
    state.centre.z+=std::sin(heading)*
        state.forwardSpeedMetersPerSecond*dt;
    return result;
}
} // namespace

RoadVehicleFixtureReport RunRoadVehicleFixture(
    const RoadContactSceneOwner& scene,
    const RoadVehicleFixtureRequest& request)
{
    RoadVehicleFixtureReport result;
    const auto* active=scene.Snapshot();
    if(!active || request.frame!=active->frame)
    {
        result.error="No active contact scene in the requested reference frame.";
        return result;
    }
    result.frame=active->frame;
    result.worldGeneration=active->worldGeneration;
    result.contactPacketSignature=active->packetSignature;
    result.finalState=request.initial;
    if(!Valid(request.vehicle) || !Valid(request.initial) ||
       !Finite(request.control.throttle) ||
       request.control.throttle<0.0 || request.control.throttle>1.0 ||
       !Finite(request.control.brake) ||
       request.control.brake<0.0 || request.control.brake>1.0 ||
       !Finite(request.control.steerRadians) ||
       std::abs(request.control.steerRadians)>0.6 ||
       !Finite(request.fixedStepSeconds) ||
       request.fixedStepSeconds<1.0/480.0 ||
       request.fixedStepSeconds>1.0/30.0 ||
       request.steps==0 || request.steps>2400 ||
       request.maximumTrianglesPerWheel==0 ||
       request.maximumTrianglesPerWheel>4096)
    {
        result.error="Invalid or excessive fixed-step vehicle test configuration.";
        return result;
    }
    std::array<scene::ObjectId,4> lastOwners{};
    for(u32 i=0;i<request.steps;++i)
    {
        RoadVehicleStepReport contact;
        try
        {
            contact=Step(scene,request.frame,request.vehicle,request.control,
                request.fixedStepSeconds,request.maximumTrianglesPerWheel,
                result.finalState);
        }
        catch(const std::invalid_argument& e)
        {
            result.error=e.what();
            return result;
        }
        if(contact.queryBudgetExceeded)
        {
            result.error="Wheel ray contact query exhausted triangle budget.";
            return result;
        }
        if(!Valid(result.finalState))
        {
            result.error="Vehicle integration became nonfinite or unstable.";
            return result;
        }
        result.maximumBottomOutMeters=std::max(
            result.maximumBottomOutMeters,contact.maximumBottomOutMeters);
        result.maximumAbsVerticalAcceleration=std::max(
            result.maximumAbsVerticalAcceleration,
            std::abs(contact.verticalAccelerationMetersPerSecond2));
        result.maximumAbsPitchRadians=std::max(
            result.maximumAbsPitchRadians,
            std::abs(result.finalState.pitchRadians));
        result.maximumAbsRollRadians=std::max(
            result.maximumAbsRollRadians,
            std::abs(result.finalState.rollRadians));
        if(contact.contactingWheels>0)++result.stepsWithGroundContact;
        if(contact.contactingWheels==4)++result.stepsWithFullContact;
        if(contact.patchWheels>0)++result.stepsWithPatchContact;
        if(contact.roadWheels>0)++result.stepsWithRoadContact;
        for(std::size_t w=0;w<4;++w)
        {
            if(contact.wheelOwners[w] && lastOwners[w] &&
               contact.wheelOwners[w]!=lastOwners[w])
                ++result.wheelOwnerTransitions;
            lastOwners[w]=contact.wheelOwners[w];
        }
        result.pathDistanceMeters+=
            result.finalState.forwardSpeedMetersPerSecond*
            request.fixedStepSeconds;
        ++result.simulatedSteps;
    }
    result.completed=true;
    return result;
}
} // namespace orbit::path_geometry

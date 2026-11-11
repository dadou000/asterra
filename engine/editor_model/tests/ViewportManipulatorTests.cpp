#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/editor_model/BuiltinSchemas.hpp>
#include <orbit/editor_model/ViewportManipulator.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <source_location>
#include <variant>

namespace
{
using namespace orbit;
using namespace orbit::editor_model;

void Check(
    const bool condition,
    const std::source_location location =
        std::source_location::current())
{
    if (!condition)
    {
        std::cerr << "ViewportManipulator test failed at line "
                  << location.line() << '\n';
        std::exit(1);
    }
}

bool Near(
    const f64 a,
    const f64 b,
    const f64 epsilon = 1.0e-6)
{
    return std::abs(a - b) <= epsilon;
}

bool Near(
    const math::Double3& a,
    const math::Double3& b,
    const f64 epsilon = 1.0e-6)
{
    return Near(a.x, b.x, epsilon) &&
        Near(a.y, b.y, epsilon) &&
        Near(a.z, b.z, epsilon);
}

// A ray from `eye` through `through`.
ManipulatorRay Through(
    const math::Double3& eye,
    const math::Double3& through)
{
    return {eye, math::Normalize(through - eye)};
}

math::Double3 Vec3(
    const scene::ObjectStore& objects,
    const scene::ObjectId object,
    const schema::PropertyId property)
{
    const auto value = objects.GetProperty(object, property);
    Check(value.has_value());
    return std::get<math::Double3>(*value);
}

f64 Float(
    const scene::ObjectStore& objects,
    const scene::ObjectId object,
    const schema::PropertyId property)
{
    const auto value = objects.GetProperty(object, property);
    Check(value.has_value());
    return std::get<f64>(*value);
}

void MathTests()
{
    // Euler <-> rotation round trips, including near gimbal lock.
    for (const math::Double3 euler :
         {math::Double3{0.0, 0.0, 0.0},
          math::Double3{30.0, 20.0, 10.0},
          math::Double3{-120.0, 45.0, 170.0},
          math::Double3{10.0, 89.9, -40.0}})
    {
        const auto rotation = EulerDegreesToRotation(euler);
        const auto back = RotationToEulerDegrees(rotation);
        const auto again = EulerDegreesToRotation(back);
        Check(Near(rotation.xAxis, again.xAxis, 1.0e-6));
        Check(Near(rotation.yAxis, again.yAxis, 1.0e-6));
        Check(Near(rotation.zAxis, again.zAxis, 1.0e-6));
    }

    // Rotation about +Z by 90 degrees maps X to Y.
    const auto quarter =
        RotationAboutAxis({0.0, 0.0, 1.0}, 1.5707963267948966);
    Check(Near(quarter.xAxis, {0.0, 1.0, 0.0}, 1.0e-9));

    // Closest point on the X axis to a ray through (3,0,0).
    const auto t =
        ClosestAxisParameter(
            Through({0.0, 5.0, -10.0}, {3.0, 0.0, 0.0}),
            {0.0, 0.0, 0.0},
            {1.0, 0.0, 0.0});
    Check(t.has_value() && Near(*t, 3.0, 1.0e-9));

    // A ray parallel to the axis has no closest parameter.
    Check(!ClosestAxisParameter(
               {{0.0, 1.0, 0.0}, {1.0, 0.0, 0.0}},
               {0.0, 0.0, 0.0},
               {1.0, 0.0, 0.0})
               .has_value());
}
} // namespace

int main()
{
    MathTests();

    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-viewport-manipulator-" +
         documents::ProjectId::Random().ToString());
    std::filesystem::remove_all(root);

    {
        auto project =
            documents::ProjectDocument::Create(
                root,
                "Manipulator Test");
        documents::WorldDatabase world(project.StartupWorldPath());

        schema::SchemaRegistry schemas;
        builtin::RegisterSchemas(schemas);

        scene::ObjectStore objects(world);
        commands::CommandService commands(objects, schemas);

        const auto worldObject =
            commands.CreateObject(world_model::kWorldType, "World");

        ViewportManipulator manipulator(objects, commands);
        const math::Double3 eye{0.0, 0.0, -10.0};
        const math::Double3 axisX{1.0, 0.0, 0.0};

        // --- Translate along X -------------------------------------------
        const auto box =
            commands.CreateObject(
                world_model::kPrimitiveType, "Box", worldObject);
        commands.SetProperty(
            box,
            world_model::kPrimitivePositionMeters,
            math::Double3{1.0, 2.0, 3.0});

        auto target = ResolveManipulatorTarget(objects, box);
        Check(target.has_value());
        Check(target->canTranslate && target->canRotate && target->canScale);
        Check(Near(target->position, {1.0, 2.0, 3.0}));

        // The handle axis passes through the position; grab it 1 m along.
        const math::Double3 centre = target->position;
        Check(manipulator.Begin(
            *target,
            ManipulatorTool::Translate,
            ManipulatorSpace::World,
            ManipulatorAxis::X,
            Through(eye, centre + axisX * 1.0)));
        Check(manipulator.Active());
        Check(commands.HasActiveTransaction());

        manipulator.Update(
            Through(eye, centre + axisX * 3.5), ManipulatorSnap{});
        Check(Near(
            Vec3(objects, box, world_model::kPrimitivePositionMeters),
            {3.5, 2.0, 3.0}));
        Check(manipulator.Summary().find("Move X") == 0);

        // Snapping rounds the displacement (2.5 -> 3).
        ManipulatorSnap snap;
        snap.translate = true;
        snap.translateMeters = 1.0;
        manipulator.Update(
            Through(eye, centre + axisX * 3.5), snap);
        Check(Near(
            Vec3(objects, box, world_model::kPrimitivePositionMeters),
            {4.0, 2.0, 3.0}));

        manipulator.Commit();
        Check(!manipulator.Active());
        Check(!commands.HasActiveTransaction());

        // The whole drag, however many updates, is one undo step.
        commands.Undo();
        Check(Near(
            Vec3(objects, box, world_model::kPrimitivePositionMeters),
            {1.0, 2.0, 3.0}));

        // Cancel restores the object exactly.
        target = ResolveManipulatorTarget(objects, box);
        Check(manipulator.Begin(
            *target,
            ManipulatorTool::Translate,
            ManipulatorSpace::World,
            ManipulatorAxis::Y,
            Through(eye, target->position)));
        manipulator.Update(
            Through(eye, target->position + math::Double3{0.0, 5.0, 0.0}),
            ManipulatorSnap{});
        manipulator.Cancel();
        Check(!manipulator.Active());
        Check(!commands.HasActiveTransaction());
        Check(Near(
            Vec3(objects, box, world_model::kPrimitivePositionMeters),
            {1.0, 2.0, 3.0}));

        // A handle the ray runs parallel to cannot be grabbed.
        Check(!manipulator.Begin(
            *target,
            ManipulatorTool::Translate,
            ManipulatorSpace::World,
            ManipulatorAxis::Z,
            {{1.0, 2.0, -10.0}, {0.0, 0.0, 1.0}}));
        Check(!commands.HasActiveTransaction());

        // --- Rotate ---------------------------------------------------------
        commands.SetProperty(
            box,
            world_model::kPrimitivePositionMeters,
            math::Double3{0.0, 0.0, 0.0});
        target = ResolveManipulatorTarget(objects, box);

        Check(manipulator.Begin(
            *target,
            ManipulatorTool::Rotate,
            ManipulatorSpace::World,
            ManipulatorAxis::Z,
            Through(eye, {2.0, 0.0, 0.0})));
        manipulator.Update(
            Through(eye, {0.0, 2.0, 0.0}), ManipulatorSnap{});
        Check(Near(
            Vec3(objects, box, world_model::kPrimitiveEulerDegrees),
            {0.0, 0.0, 90.0},
            1.0e-6));

        ManipulatorSnap rotateSnap;
        rotateSnap.rotate = true;
        rotateSnap.rotateDegrees = 15.0;
        // 40 degrees -> 45.
        manipulator.Update(
            Through(eye, {std::cos(0.6981317007977318) * 2.0,
                          std::sin(0.6981317007977318) * 2.0, 0.0}),
            rotateSnap);
        Check(Near(
            Vec3(objects, box, world_model::kPrimitiveEulerDegrees),
            {0.0, 0.0, 45.0},
            1.0e-6));
        manipulator.Commit();

        // Local space rotates about the object's own axis: after the 45
        // degree roll, local X is no longer world X.
        target = ResolveManipulatorTarget(objects, box);
        const auto localX =
            ManipulatorAxisDirection(
                *target,
                ManipulatorTool::Rotate,
                ManipulatorSpace::Local,
                ManipulatorAxis::X);
        Check(Near(localX, {std::sqrt(0.5), std::sqrt(0.5), 0.0}, 1.0e-9));
        // Scale ignores the requested space and uses local axes.
        Check(EffectiveSpace(
                  ManipulatorTool::Scale, ManipulatorSpace::World) ==
              ManipulatorSpace::Local);

        // --- Scale ----------------------------------------------------------
        commands.SetProperty(
            box,
            world_model::kPrimitiveEulerDegrees,
            math::Double3{0.0, 0.0, 0.0});
        target = ResolveManipulatorTarget(objects, box);

        Check(manipulator.Begin(
            *target,
            ManipulatorTool::Scale,
            ManipulatorSpace::Local,
            ManipulatorAxis::X,
            Through(eye, {2.0, 0.0, 0.0})));
        manipulator.Update(
            Through(eye, {4.0, 0.0, 0.0}), ManipulatorSnap{});
        Check(Near(
            Vec3(objects, box, world_model::kPrimitiveSizeMeters),
            {2.0, 1.0, 1.0}));
        manipulator.Commit();

        // Uniform scale from a screen-facing plane: 1 m -> 3 m is x3,
        // snapped to 0.5 steps and never below one step.
        commands.SetProperty(
            box,
            world_model::kPrimitiveSizeMeters,
            math::Double3{1.0, 1.0, 1.0});
        target = ResolveManipulatorTarget(objects, box);
        Check(manipulator.Begin(
            *target,
            ManipulatorTool::Scale,
            ManipulatorSpace::Local,
            ManipulatorAxis::Uniform,
            Through(eye, {1.0, 0.0, 0.0})));
        ManipulatorSnap scaleSnap;
        scaleSnap.scale = true;
        scaleSnap.scaleStep = 0.5;
        manipulator.Update(Through(eye, {3.2, 0.0, 0.0}), scaleSnap);
        Check(Near(
            Vec3(objects, box, world_model::kPrimitiveSizeMeters),
            {3.0, 3.0, 3.0}));
        manipulator.Update(Through(eye, {0.01, 0.0, 0.0}), scaleSnap);
        Check(Near(
            Vec3(objects, box, world_model::kPrimitiveSizeMeters),
            {0.5, 0.5, 0.5}));
        manipulator.Cancel();
        Check(Near(
            Vec3(objects, box, world_model::kPrimitiveSizeMeters),
            {1.0, 1.0, 1.0}));

        // Uniform is not a translate or rotate handle.
        Check(!manipulator.Begin(
            *target,
            ManipulatorTool::Translate,
            ManipulatorSpace::World,
            ManipulatorAxis::Uniform,
            Through(eye, {1.0, 0.0, 0.0})));

        // --- Visibility proxy: sphere radius, box half extents -------------
        const auto proxy =
            commands.CreateObject(
                world_model::kVisibilityProxyType, "Proxy", worldObject);
        commands.SetProperty(
            proxy, world_model::kVisibilityProxyShape, i64{0});
        commands.SetProperty(
            proxy, world_model::kVisibilityProxyRadiusMeters, 0.5);
        target = ResolveManipulatorTarget(objects, proxy);
        Check(target.has_value() && target->canScale);
        Check(manipulator.Begin(
            *target,
            ManipulatorTool::Scale,
            ManipulatorSpace::Local,
            ManipulatorAxis::Uniform,
            Through(eye, {0.5, 0.0, 0.0})));
        manipulator.Update(Through(eye, {1.5, 0.0, 0.0}), ManipulatorSnap{});
        // The drag plane is perpendicular to the grab ray, so a unit step on
        // screen is only approximately the same step in the world.
        Check(Near(
            Float(objects, proxy, world_model::kVisibilityProxyRadiusMeters),
            1.5,
            0.05));
        manipulator.Commit();

        commands.SetProperty(
            proxy, world_model::kVisibilityProxyShape, i64{1});
        target = ResolveManipulatorTarget(objects, proxy);
        Check(manipulator.Begin(
            *target,
            ManipulatorTool::Scale,
            ManipulatorSpace::Local,
            ManipulatorAxis::Y,
            Through(eye, {0.0, 1.0, 0.0})));
        manipulator.Update(Through(eye, {0.0, 2.0, 0.0}), ManipulatorSnap{});
        Check(Near(
            Vec3(objects, proxy,
                 world_model::kVisibilityProxyHalfExtentsMeters),
            {0.5, 1.0, 0.5}));
        manipulator.Cancel();

        // --- Lights ---------------------------------------------------------
        const auto point =
            commands.CreateObject(
                world_model::kPointLightType, "Point", worldObject);
        target = ResolveManipulatorTarget(objects, point);
        Check(target.has_value());
        Check(target->canTranslate && !target->canRotate && !target->canScale);
        Check(!manipulator.Begin(
            *target,
            ManipulatorTool::Rotate,
            ManipulatorSpace::World,
            ManipulatorAxis::X,
            Through(eye, {0.0, 1.0, 0.0})));
        Check(!manipulator.Begin(
            *target,
            ManipulatorTool::Scale,
            ManipulatorSpace::Local,
            ManipulatorAxis::X,
            Through(eye, {1.0, 0.0, 0.0})));

        const auto spot =
            commands.CreateObject(
                world_model::kSpotLightType, "Spot", worldObject);
        commands.SetProperty(
            spot,
            world_model::kLightDirection,
            math::Double3{0.0, -1.0, 0.0});
        target = ResolveManipulatorTarget(objects, spot);
        Check(target.has_value() && target->canRotate);

        // Look along +X at the plane x = 0; rotate +90 degrees about world
        // X, which takes the down direction to -Z.
        const math::Double3 sideEye{-10.0, 0.0, 0.0};
        Check(manipulator.Begin(
            *target,
            ManipulatorTool::Rotate,
            ManipulatorSpace::World,
            ManipulatorAxis::X,
            Through(sideEye, {0.0, 2.0, 0.0})));
        manipulator.Update(
            Through(sideEye, {0.0, 0.0, 2.0}), ManipulatorSnap{});
        Check(Near(
            Vec3(objects, spot, world_model::kLightDirection),
            {0.0, 0.0, -1.0},
            1.0e-6));
        manipulator.Commit();

        // --- Headless ApplyDelta (RPC/MCP) ----------------------------------
        commands.SetProperty(
            box,
            world_model::kPrimitivePositionMeters,
            math::Double3{1.0, 0.0, 0.0});
        commands.SetProperty(
            box,
            world_model::kPrimitiveEulerDegrees,
            math::Double3{0.0, 0.0, 0.0});
        commands.SetProperty(
            box,
            world_model::kPrimitiveSizeMeters,
            math::Double3{1.0, 1.0, 1.0});

        target = ResolveManipulatorTarget(objects, box);
        const auto moveSummary =
            manipulator.ApplyDelta(
                *target,
                ManipulatorTool::Translate,
                ManipulatorSpace::World,
                ManipulatorAxis::Y,
                2.5);
        Check(moveSummary.find("Move Y") == 0);
        Check(Near(
            Vec3(objects, box, world_model::kPrimitivePositionMeters),
            {1.0, 2.5, 0.0}));
        Check(!commands.HasActiveTransaction());

        target = ResolveManipulatorTarget(objects, box);
        static_cast<void>(
            manipulator.ApplyDelta(
                *target,
                ManipulatorTool::Rotate,
                ManipulatorSpace::World,
                ManipulatorAxis::Z,
                90.0));
        Check(Near(
            Vec3(objects, box, world_model::kPrimitiveEulerDegrees),
            {0.0, 0.0, 90.0}));

        // Local-space translate follows the rotated axis: local X is now
        // world Y.
        target = ResolveManipulatorTarget(objects, box);
        static_cast<void>(
            manipulator.ApplyDelta(
                *target,
                ManipulatorTool::Translate,
                ManipulatorSpace::Local,
                ManipulatorAxis::X,
                1.0));
        Check(Near(
            Vec3(objects, box, world_model::kPrimitivePositionMeters),
            {1.0, 3.5, 0.0}));

        static_cast<void>(
            manipulator.ApplyDelta(
                *target,
                ManipulatorTool::Scale,
                ManipulatorSpace::Local,
                ManipulatorAxis::Uniform,
                2.0));
        Check(Near(
            Vec3(objects, box, world_model::kPrimitiveSizeMeters),
            {2.0, 2.0, 2.0}));

        // Each ApplyDelta is one undo step.
        commands.Undo();
        Check(Near(
            Vec3(objects, box, world_model::kPrimitiveSizeMeters),
            {1.0, 1.0, 1.0}));

        // Invalid requests throw and change nothing.
        const auto before =
            Vec3(objects, box, world_model::kPrimitivePositionMeters);
        bool threw = false;
        try
        {
            static_cast<void>(
                manipulator.ApplyDelta(
                    *target,
                    ManipulatorTool::Scale,
                    ManipulatorSpace::Local,
                    ManipulatorAxis::X,
                    -1.0));
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }
        Check(threw);
        threw = false;
        try
        {
            const auto pointTarget =
                ResolveManipulatorTarget(objects, point);
            static_cast<void>(
                manipulator.ApplyDelta(
                    *pointTarget,
                    ManipulatorTool::Rotate,
                    ManipulatorSpace::World,
                    ManipulatorAxis::X,
                    10.0));
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }
        Check(threw);
        Check(!commands.HasActiveTransaction());
        Check(Near(
            Vec3(objects, box, world_model::kPrimitivePositionMeters),
            before));

        // --- Not manipulable -----------------------------------------------
        Check(!ResolveManipulatorTarget(objects, worldObject).has_value());
        Check(!ResolveManipulatorTarget(objects, scene::ObjectId{})
                   .has_value());
    }

    std::filesystem::remove_all(root);
    return 0;
}

#include <orbit/editor_model/ViewportManipulator.hpp>

#include <orbit/world_model/WorldSchemas.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>
#include <variant>

namespace orbit::editor_model
{
namespace
{
constexpr f64 kDegreesToRadians = 0.017453292519943295769;
constexpr f64 kRadiansToDegrees = 57.295779513082320877;
constexpr f64 kMinimumScaleFactor = 0.001;
constexpr f64 kMaximumScaleFactor = 1000.0;
constexpr f64 kMinimumSizeMeters = 0.001;
constexpr f64 kParallelEpsilon = 1.0e-8;

template <typename T>
[[nodiscard]] std::optional<T> ReadProperty(
    const scene::ObjectStore& objects,
    const scene::ObjectId object,
    const schema::PropertyId property)
{
    const auto value =
        objects.GetProperty(
            object,
            property);

    if (!value.has_value())
    {
        return std::nullopt;
    }

    if (const auto* typed =
            std::get_if<T>(&*value))
    {
        return *typed;
    }

    return std::nullopt;
}

template <typename T>
[[nodiscard]] T ReadPropertyOr(
    const scene::ObjectStore& objects,
    const scene::ObjectId object,
    const schema::PropertyId property,
    const T& fallback)
{
    return ReadProperty<T>(
               objects,
               object,
               property)
        .value_or(fallback);
}

[[nodiscard]] bool Finite(
    const math::Double3& value) noexcept
{
    return std::isfinite(value.x) &&
        std::isfinite(value.y) &&
        std::isfinite(value.z);
}

[[nodiscard]] math::Double3x3 BasisFromDirection(
    const math::Double3& direction) noexcept
{
    math::Double3 z = direction;

    if (math::LengthSquared(z) <= 1.0e-12)
    {
        z = {0.0, -1.0, 0.0};
    }

    z = math::Normalize(z);

    math::Double3 hint{0.0, 1.0, 0.0};

    if (std::abs(math::Dot(z, hint)) > 0.95)
    {
        hint = {1.0, 0.0, 0.0};
    }

    const math::Double3 x =
        math::Normalize(math::Cross(hint, z));
    const math::Double3 y =
        math::Cross(z, x);

    return {
        .xAxis = x,
        .yAxis = y,
        .zAxis = z
    };
}

[[nodiscard]] math::Double3 AxisOf(
    const math::Double3x3& rotation,
    const ManipulatorAxis axis) noexcept
{
    switch (axis)
    {
    case ManipulatorAxis::X:
        return rotation.xAxis;
    case ManipulatorAxis::Y:
        return rotation.yAxis;
    case ManipulatorAxis::Z:
        return rotation.zAxis;
    case ManipulatorAxis::Uniform:
        break;
    }

    return {};
}

[[nodiscard]] f64 Component(
    const math::Double3& value,
    const ManipulatorAxis axis) noexcept
{
    switch (axis)
    {
    case ManipulatorAxis::X:
        return value.x;
    case ManipulatorAxis::Y:
        return value.y;
    case ManipulatorAxis::Z:
        return value.z;
    case ManipulatorAxis::Uniform:
        break;
    }

    return 0.0;
}

[[nodiscard]] math::Double3 WithComponent(
    math::Double3 value,
    const ManipulatorAxis axis,
    const f64 component) noexcept
{
    switch (axis)
    {
    case ManipulatorAxis::X:
        value.x = component;
        break;
    case ManipulatorAxis::Y:
        value.y = component;
        break;
    case ManipulatorAxis::Z:
        value.z = component;
        break;
    case ManipulatorAxis::Uniform:
        break;
    }

    return value;
}

[[nodiscard]] char AxisName(
    const ManipulatorAxis axis) noexcept
{
    switch (axis)
    {
    case ManipulatorAxis::X:
        return 'X';
    case ManipulatorAxis::Y:
        return 'Y';
    case ManipulatorAxis::Z:
        return 'Z';
    case ManipulatorAxis::Uniform:
        break;
    }

    return 'U';
}

[[nodiscard]] f64 Snapped(
    const f64 value,
    const f64 step) noexcept
{
    if (!(step > 0.0) ||
        !std::isfinite(step))
    {
        return value;
    }

    return std::round(value / step) * step;
}

[[nodiscard]] schema::PropertyId PositionProperty(
    const ManipulatedKind kind) noexcept
{
    switch (kind)
    {
    case ManipulatedKind::Primitive:
        return world_model::kPrimitivePositionMeters;
    case ManipulatedKind::VisibilityProxy:
        return world_model::kVisibilityProxyPositionMeters;
    case ManipulatedKind::StaticMesh:
        return world_model::kStaticMeshPositionMeters;
    case ManipulatedKind::PointLight:
    case ManipulatedKind::SpotLight:
        break;
    }

    return world_model::kLightPositionMeters;
}

[[nodiscard]] schema::PropertyId EulerProperty(
    const ManipulatedKind kind) noexcept
{
    if (kind == ManipulatedKind::StaticMesh)
    {
        return world_model::kStaticMeshEulerDegrees;
    }

    return kind == ManipulatedKind::Primitive
        ? world_model::kPrimitiveEulerDegrees
        : world_model::kVisibilityProxyEulerDegrees;
}

[[nodiscard]] bool UsesEuler(
    const ManipulatedKind kind) noexcept
{
    return kind == ManipulatedKind::Primitive ||
        kind == ManipulatedKind::VisibilityProxy ||
        kind == ManipulatedKind::StaticMesh;
}
} // namespace

math::Double3x3 EulerDegreesToRotation(
    const math::Double3& eulerDegrees) noexcept
{
    const f64 rx = eulerDegrees.x * kDegreesToRadians;
    const f64 ry = eulerDegrees.y * kDegreesToRadians;
    const f64 rz = eulerDegrees.z * kDegreesToRadians;

    const f64 cx = std::cos(rx);
    const f64 sx = std::sin(rx);
    const f64 cy = std::cos(ry);
    const f64 sy = std::sin(ry);
    const f64 cz = std::cos(rz);
    const f64 sz = std::sin(rz);

    // Intrinsic XYZ (parent-space Rz * Ry * Rx); identical to the
    // convention the viewport renderer uses for visibility proxies.
    return {
        .xAxis = {cz * cy, sz * cy, -sy},
        .yAxis = {
            cz * sy * sx - sz * cx,
            sz * sy * sx + cz * cx,
            cy * sx},
        .zAxis = {
            cz * sy * cx + sz * sx,
            sz * sy * cx - cz * sx,
            cy * cx}
    };
}

math::Double3 RotationToEulerDegrees(
    const math::Double3x3& rotation) noexcept
{
    const f64 sinY =
        std::clamp(-rotation.xAxis.z, -1.0, 1.0);
    const f64 ry = std::asin(sinY);
    const f64 cosY = std::cos(ry);

    f64 rx = 0.0;
    f64 rz = 0.0;

    if (std::abs(cosY) > 1.0e-6)
    {
        rz = std::atan2(
            rotation.xAxis.y,
            rotation.xAxis.x);
        rx = std::atan2(
            rotation.yAxis.z,
            rotation.zAxis.z);
    }
    else
    {
        // Gimbal lock: fold the whole roll into X.
        rx = std::atan2(
            -rotation.zAxis.y,
            rotation.yAxis.y);
    }

    return {
        rx * kRadiansToDegrees,
        ry * kRadiansToDegrees,
        rz * kRadiansToDegrees
    };
}

math::Double3x3 RotationAboutAxis(
    const math::Double3& unitAxis,
    const f64 radians) noexcept
{
    const f64 c = std::cos(radians);
    const f64 s = std::sin(radians);

    const auto rotate =
        [&](const math::Double3& v)
        {
            return v * c +
                math::Cross(unitAxis, v) * s +
                unitAxis *
                    (math::Dot(unitAxis, v) * (1.0 - c));
        };

    return {
        .xAxis = rotate({1.0, 0.0, 0.0}),
        .yAxis = rotate({0.0, 1.0, 0.0}),
        .zAxis = rotate({0.0, 0.0, 1.0})
    };
}

std::optional<f64> ClosestAxisParameter(
    const ManipulatorRay& ray,
    const math::Double3& axisOrigin,
    const math::Double3& unitAxis) noexcept
{
    const math::Double3 e =
        math::Normalize(ray.direction);

    if (math::LengthSquared(e) <= 1.0e-20 ||
        math::LengthSquared(unitAxis) <= 1.0e-20)
    {
        return std::nullopt;
    }

    const math::Double3 w0 = axisOrigin - ray.origin;
    const f64 b = math::Dot(unitAxis, e);
    const f64 d = math::Dot(unitAxis, w0);
    const f64 f = math::Dot(e, w0);
    const f64 denominator = 1.0 - b * b;

    if (denominator < kParallelEpsilon)
    {
        return std::nullopt;
    }

    const f64 t = (b * f - d) / denominator;

    if (!std::isfinite(t))
    {
        return std::nullopt;
    }

    return t;
}

std::optional<math::Double3> IntersectRayPlane(
    const ManipulatorRay& ray,
    const math::Double3& planePoint,
    const math::Double3& planeNormal) noexcept
{
    const math::Double3 e =
        math::Normalize(ray.direction);
    const f64 denominator =
        math::Dot(planeNormal, e);

    if (std::abs(denominator) < 1.0e-6)
    {
        return std::nullopt;
    }

    const f64 t =
        math::Dot(planeNormal, planePoint - ray.origin) /
        denominator;

    if (!std::isfinite(t) || t < 0.0)
    {
        return std::nullopt;
    }

    return ray.origin + e * t;
}

std::optional<ManipulatorTarget>
ResolveManipulatorTarget(
    const scene::ObjectStore& objects,
    const scene::ObjectId object)
{
    const auto record = objects.Find(object);

    if (!record.has_value())
    {
        return std::nullopt;
    }

    ManipulatorTarget target{.object = object};

    if (record->type == world_model::kPrimitiveType)
    {
        target.kind = ManipulatedKind::Primitive;
        target.position =
            ReadPropertyOr<math::Double3>(
                objects, object,
                world_model::kPrimitivePositionMeters, {});
        target.rotation = EulerDegreesToRotation(
            ReadPropertyOr<math::Double3>(
                objects, object,
                world_model::kPrimitiveEulerDegrees, {}));
        target.canTranslate = true;
        target.canRotate = true;
        target.canScale = true;
    }
    else if (record->type == world_model::kStaticMeshType)
    {
        target.kind = ManipulatedKind::StaticMesh;
        target.position =
            ReadPropertyOr<math::Double3>(
                objects, object,
                world_model::kStaticMeshPositionMeters, {});
        target.rotation = EulerDegreesToRotation(
            ReadPropertyOr<math::Double3>(
                objects, object,
                world_model::kStaticMeshEulerDegrees, {}));
        target.canTranslate = true;
        target.canRotate = true;
        target.canScale = true;
    }
    else if (record->type == world_model::kVisibilityProxyType)
    {
        target.kind = ManipulatedKind::VisibilityProxy;
        target.position =
            ReadPropertyOr<math::Double3>(
                objects, object,
                world_model::kVisibilityProxyPositionMeters, {});
        target.rotation = EulerDegreesToRotation(
            ReadPropertyOr<math::Double3>(
                objects, object,
                world_model::kVisibilityProxyEulerDegrees, {}));
        target.canTranslate = true;
        target.canRotate = true;
        target.canScale = true;
    }
    else if (record->type == world_model::kPointLightType)
    {
        target.kind = ManipulatedKind::PointLight;
        target.position =
            ReadPropertyOr<math::Double3>(
                objects, object,
                world_model::kLightPositionMeters, {});
        target.canTranslate = true;
    }
    else if (record->type == world_model::kSpotLightType)
    {
        target.kind = ManipulatedKind::SpotLight;
        target.position =
            ReadPropertyOr<math::Double3>(
                objects, object,
                world_model::kLightPositionMeters, {});
        target.rotation = BasisFromDirection(
            ReadPropertyOr<math::Double3>(
                objects, object,
                world_model::kLightDirection,
                {0.0, -1.0, 0.0}));
        target.canTranslate = true;
        target.canRotate = true;
    }
    else
    {
        return std::nullopt;
    }

    if (!Finite(target.position))
    {
        return std::nullopt;
    }

    return target;
}

bool Supports(
    const ManipulatorTarget& target,
    const ManipulatorTool tool) noexcept
{
    switch (tool)
    {
    case ManipulatorTool::Translate:
        return target.canTranslate;
    case ManipulatorTool::Rotate:
        return target.canRotate;
    case ManipulatorTool::Scale:
        return target.canScale;
    }

    return false;
}

ManipulatorSpace EffectiveSpace(
    const ManipulatorTool tool,
    const ManipulatorSpace requested) noexcept
{
    return tool == ManipulatorTool::Scale
        ? ManipulatorSpace::Local
        : requested;
}

math::Double3 ManipulatorAxisDirection(
    const ManipulatorTarget& target,
    const ManipulatorTool tool,
    const ManipulatorSpace space,
    const ManipulatorAxis axis) noexcept
{
    if (axis == ManipulatorAxis::Uniform)
    {
        return {};
    }

    if (EffectiveSpace(tool, space) ==
        ManipulatorSpace::Local)
    {
        return AxisOf(target.rotation, axis);
    }

    return AxisOf(math::Identity3D(), axis);
}

ViewportManipulator::ViewportManipulator(
    scene::ObjectStore& objects,
    commands::CommandService& commands) noexcept
    : objects_(objects),
      commands_(commands)
{
}

ViewportManipulator::~ViewportManipulator()
{
    Cancel();
}

void ViewportManipulator::CaptureStartValues(
    const ManipulatorTarget& target,
    const ManipulatorTool tool)
{
    startEuler_ = {};
    startSize_ = {};
    startRadius_ = 0.0;
    sphereProxy_ = false;
    startDirection_ = {};

    if (UsesEuler(target.kind))
    {
        startEuler_ =
            ReadPropertyOr<math::Double3>(
                objects_, target.object,
                EulerProperty(target.kind), {});
    }

    if (target.kind == ManipulatedKind::SpotLight)
    {
        startDirection_ = target.rotation.zAxis;
    }

    if (tool != ManipulatorTool::Scale)
    {
        return;
    }

    if (target.kind == ManipulatedKind::StaticMesh)
    {
        // The uniform scale is kept in startRadius_ (a single scalar).
        startRadius_ =
            ReadPropertyOr<f64>(
                objects_, target.object,
                world_model::kStaticMeshScale, 1.0);
        return;
    }

    if (target.kind == ManipulatedKind::Primitive)
    {
        startSize_ =
            ReadPropertyOr<math::Double3>(
                objects_, target.object,
                world_model::kPrimitiveSizeMeters,
                {1.0, 1.0, 1.0});
        return;
    }

    sphereProxy_ =
        ReadPropertyOr<i64>(
            objects_, target.object,
            world_model::kVisibilityProxyShape,
            i64{0}) != 1;

    if (sphereProxy_)
    {
        startRadius_ =
            ReadPropertyOr<f64>(
                objects_, target.object,
                world_model::kVisibilityProxyRadiusMeters,
                0.5);
    }
    else
    {
        startSize_ =
            ReadPropertyOr<math::Double3>(
                objects_, target.object,
                world_model::kVisibilityProxyHalfExtentsMeters,
                {0.5, 0.5, 0.5});
    }
}

bool ViewportManipulator::Begin(
    const ManipulatorTarget& target,
    const ManipulatorTool tool,
    const ManipulatorSpace space,
    const ManipulatorAxis axis,
    const ManipulatorRay& grabRay)
{
    if (active_ || commands_.HasActiveTransaction())
    {
        throw std::logic_error(
            "Cannot start a viewport manipulation inside another transaction.");
    }

    if (!Supports(target, tool))
    {
        return false;
    }

    if (axis == ManipulatorAxis::Uniform &&
        tool != ManipulatorTool::Scale)
    {
        return false;
    }

    const math::Double3 direction =
        math::Normalize(grabRay.direction);

    if (math::LengthSquared(direction) <= 1.0e-20 ||
        !objects_.Find(target.object).has_value())
    {
        return false;
    }

    ManipulatorRay ray{grabRay.origin, direction};

    const math::Double3 axisDirection =
        ManipulatorAxisDirection(target, tool, space, axis);

    math::Double3 startVector{};
    math::Double3 planeNormal{};
    f64 startParameter = 0.0;
    f64 startDistance = 0.0;

    switch (tool)
    {
    case ManipulatorTool::Translate:
    {
        const auto t =
            ClosestAxisParameter(
                ray, target.position, axisDirection);

        if (!t.has_value())
        {
            return false;
        }

        startParameter = *t;
        break;
    }
    case ManipulatorTool::Rotate:
    {
        const auto hit =
            IntersectRayPlane(
                ray, target.position, axisDirection);

        if (!hit.has_value())
        {
            return false;
        }

        const math::Double3 arm = *hit - target.position;

        if (math::LengthSquared(arm) <= 1.0e-12)
        {
            return false;
        }

        startVector = math::Normalize(arm);
        planeNormal = axisDirection;
        break;
    }
    case ManipulatorTool::Scale:
    {
        if (axis == ManipulatorAxis::Uniform)
        {
            planeNormal = ray.direction * -1.0;
            const auto hit =
                IntersectRayPlane(
                    ray, target.position, planeNormal);

            if (!hit.has_value())
            {
                return false;
            }

            startDistance = math::Length(*hit - target.position);

            if (startDistance <= 1.0e-6)
            {
                return false;
            }
        }
        else
        {
            const auto t =
                ClosestAxisParameter(
                    ray, target.position, axisDirection);

            if (!t.has_value() || std::abs(*t) <= 1.0e-6)
            {
                return false;
            }

            startParameter = *t;
        }
        break;
    }
    }

    CaptureStartValues(target, tool);

    commands_.BeginTransaction(
        tool == ManipulatorTool::Translate
            ? "Move"
            : tool == ManipulatorTool::Rotate
                ? "Rotate"
                : "Scale");

    active_ = true;
    target_ = target;
    tool_ = tool;
    axis_ = axis;
    axisDirection_ = axisDirection;
    startPosition_ = target.position;
    startParameter_ = startParameter;
    startVector_ = startVector;
    planeNormal_ = planeNormal;
    startDistance_ = startDistance;
    summary_.clear();
    return true;
}

void ViewportManipulator::Update(
    const ManipulatorRay& rayIn,
    const ManipulatorSnap& snap)
{
    if (!active_)
    {
        throw std::logic_error(
            "No viewport manipulation is active.");
    }

    const ManipulatorRay ray{
        rayIn.origin,
        math::Normalize(rayIn.direction)};

    if (math::LengthSquared(ray.direction) <= 1.0e-20)
    {
        return;
    }

    switch (tool_)
    {
    case ManipulatorTool::Translate:
    {
        const auto t =
            ClosestAxisParameter(
                ray, startPosition_, axisDirection_);

        if (!t.has_value())
        {
            return;
        }

        f64 delta = *t - startParameter_;

        if (snap.translate)
        {
            delta = Snapped(delta, snap.translateMeters);
        }

        WriteAmount(delta);
        break;
    }
    case ManipulatorTool::Rotate:
    {
        const auto hit =
            IntersectRayPlane(
                ray, startPosition_, planeNormal_);

        if (!hit.has_value())
        {
            return;
        }

        const math::Double3 arm = *hit - startPosition_;

        if (math::LengthSquared(arm) <= 1.0e-12)
        {
            return;
        }

        const math::Double3 current = math::Normalize(arm);
        f64 degrees =
            std::atan2(
                math::Dot(
                    planeNormal_,
                    math::Cross(startVector_, current)),
                math::Dot(startVector_, current)) *
            kRadiansToDegrees;

        if (snap.rotate)
        {
            degrees = Snapped(degrees, snap.rotateDegrees);
        }

        WriteAmount(degrees);
        break;
    }
    case ManipulatorTool::Scale:
    {
        f64 factor = 1.0;

        if (axis_ == ManipulatorAxis::Uniform)
        {
            const auto hit =
                IntersectRayPlane(
                    ray, startPosition_, planeNormal_);

            if (!hit.has_value())
            {
                return;
            }

            factor =
                math::Length(*hit - startPosition_) /
                startDistance_;
        }
        else
        {
            const auto t =
                ClosestAxisParameter(
                    ray, startPosition_, axisDirection_);

            if (!t.has_value())
            {
                return;
            }

            factor = *t / startParameter_;
        }

        factor =
            std::clamp(
                factor,
                kMinimumScaleFactor,
                kMaximumScaleFactor);

        if (snap.scale)
        {
            factor =
                std::max(
                    Snapped(factor, snap.scaleStep),
                    std::max(snap.scaleStep, kMinimumScaleFactor));
        }

        WriteAmount(factor);
        break;
    }
    }
}

void ViewportManipulator::WriteAmount(const f64 amount)
{
    switch (tool_)
    {
    case ManipulatorTool::Translate:
    {
        commands_.SetProperty(
            target_.object,
            PositionProperty(target_.kind),
            startPosition_ + axisDirection_ * amount);

        summary_ =
            std::format(
                "Move {} {:+.3f} m",
                AxisName(axis_),
                amount);
        break;
    }
    case ManipulatorTool::Rotate:
    {
        const math::Double3x3 delta =
            RotationAboutAxis(
                axisDirection_,
                amount * kDegreesToRadians);

        if (UsesEuler(target_.kind))
        {
            commands_.SetProperty(
                target_.object,
                EulerProperty(target_.kind),
                RotationToEulerDegrees(
                    math::Multiply(
                        delta,
                        EulerDegreesToRotation(startEuler_))));
        }
        else
        {
            commands_.SetProperty(
                target_.object,
                world_model::kLightDirection,
                math::Normalize(
                    math::TransformVector(
                        delta,
                        startDirection_)));
        }

        summary_ =
            std::format(
                "Rotate {} {:+.1f} deg",
                AxisName(axis_),
                amount);
        break;
    }
    case ManipulatorTool::Scale:
    {
        const f64 factor =
            std::clamp(
                amount,
                kMinimumScaleFactor,
                kMaximumScaleFactor);

        if (target_.kind == ManipulatedKind::StaticMesh)
        {
            // A mesh scales uniformly whatever axis was grabbed.
            commands_.SetProperty(
                target_.object,
                world_model::kStaticMeshScale,
                std::max(startRadius_ * factor, 1.0e-6));
        }
        else if (target_.kind == ManipulatedKind::Primitive ||
            (target_.kind == ManipulatedKind::VisibilityProxy &&
             !sphereProxy_))
        {
            math::Double3 size = startSize_;

            if (axis_ == ManipulatorAxis::Uniform)
            {
                size = startSize_ * factor;
            }
            else
            {
                size =
                    WithComponent(
                        startSize_,
                        axis_,
                        Component(startSize_, axis_) * factor);
            }

            size.x = std::max(size.x, kMinimumSizeMeters);
            size.y = std::max(size.y, kMinimumSizeMeters);
            size.z = std::max(size.z, kMinimumSizeMeters);

            commands_.SetProperty(
                target_.object,
                target_.kind == ManipulatedKind::Primitive
                    ? world_model::kPrimitiveSizeMeters
                    : world_model::kVisibilityProxyHalfExtentsMeters,
                size);
        }
        else
        {
            commands_.SetProperty(
                target_.object,
                world_model::kVisibilityProxyRadiusMeters,
                std::max(
                    startRadius_ * factor,
                    kMinimumSizeMeters));
        }

        summary_ =
            std::format(
                "Scale {} x{:.3f}",
                AxisName(axis_),
                factor);
        break;
    }
    }
}

std::string ViewportManipulator::ApplyDelta(
    const ManipulatorTarget& target,
    const ManipulatorTool tool,
    const ManipulatorSpace space,
    const ManipulatorAxis axis,
    const f64 amount)
{
    if (active_ || commands_.HasActiveTransaction())
    {
        throw std::logic_error(
            "Cannot apply a transform inside another transaction.");
    }

    if (!Supports(target, tool))
    {
        throw std::invalid_argument(
            "The object does not support that transform tool.");
    }

    if ((axis == ManipulatorAxis::Uniform &&
         tool != ManipulatorTool::Scale) ||
        !std::isfinite(amount) ||
        !objects_.Find(target.object).has_value())
    {
        throw std::invalid_argument(
            "Invalid transform axis or amount.");
    }

    if (tool == ManipulatorTool::Scale && amount <= 0.0)
    {
        throw std::invalid_argument(
            "A scale factor must be positive.");
    }

    CaptureStartValues(target, tool);

    target_ = target;
    tool_ = tool;
    axis_ = axis;
    axisDirection_ =
        ManipulatorAxisDirection(target, tool, space, axis);
    startPosition_ = target.position;

    commands_.BeginTransaction(
        tool == ManipulatorTool::Translate
            ? "Move"
            : tool == ManipulatorTool::Rotate
                ? "Rotate"
                : "Scale");

    try
    {
        WriteAmount(amount);
        commands_.CommitTransaction();
    }
    catch (...)
    {
        if (commands_.HasActiveTransaction())
        {
            commands_.RollbackTransaction();
        }

        throw;
    }

    std::string result = summary_;
    summary_.clear();
    return result;
}

void ViewportManipulator::Commit()
{
    if (!active_)
    {
        throw std::logic_error(
            "No viewport manipulation is active.");
    }

    if (!commands_.HasActiveTransaction())
    {
        Clear();
        throw std::logic_error(
            "Viewport manipulation transaction is no longer active.");
    }

    commands_.CommitTransaction();
    Clear();
}

void ViewportManipulator::Cancel() noexcept
{
    if (active_ &&
        commands_.HasActiveTransaction())
    {
        try
        {
            commands_.RollbackTransaction();
        }
        catch (...)
        {
            // Cancel must never throw; the command path has already failed.
        }
    }

    Clear();
}

bool ViewportManipulator::Active() const noexcept
{
    return active_;
}

std::optional<scene::ObjectId>
ViewportManipulator::Object() const noexcept
{
    if (!active_)
    {
        return std::nullopt;
    }

    return target_.object;
}

const std::string& ViewportManipulator::Summary() const noexcept
{
    return summary_;
}

void ViewportManipulator::Clear() noexcept
{
    active_ = false;
    summary_.clear();
}
} // namespace orbit::editor_model

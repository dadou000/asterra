#pragma once

#include <orbit/commands/CommandService.hpp>
#include <orbit/math/RigidTransform.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/scene/ObjectStore.hpp>

#include <optional>
#include <string>

namespace orbit::editor_model
{
// UI-independent model of the viewport Move / Rotate / Scale gizmo. The
// Studio panel only projects handles, hit-tests them and feeds pointer rays
// here; every number that ends up in the world is computed and written here
// through CommandService, so the same operation is testable headless and
// reachable over RPC/MCP.
//
// Property space: authored positions, directions and Euler angles are
// treated as living in the viewport's local frame, the same assumption the
// light and volume gizmos already make.

enum class ManipulatorTool : u8
{
    Translate,
    Rotate,
    Scale
};

enum class ManipulatorSpace : u8
{
    World,
    Local
};

// Uniform is only valid for Scale.
enum class ManipulatorAxis : u8
{
    X,
    Y,
    Z,
    Uniform
};

enum class ManipulatedKind : u8
{
    Primitive,
    VisibilityProxy,
    PointLight,
    SpotLight
};

// A pointer ray in the same frame as the target (camera-local meters).
struct ManipulatorRay
{
    math::Double3 origin{};
    math::Double3 direction{};
};

struct ManipulatorSnap
{
    bool translate{false};
    f64 translateMeters{1.0};
    bool rotate{false};
    f64 rotateDegrees{15.0};
    bool scale{false};
    f64 scaleStep{0.1};
};

struct ManipulatorTarget
{
    scene::ObjectId object{};
    ManipulatedKind kind{ManipulatedKind::Primitive};
    math::Double3 position{};
    // Local axes expressed in the property frame (identity when the object
    // has no orientation of its own).
    math::Double3x3 rotation{};
    bool canTranslate{false};
    bool canRotate{false};
    bool canScale{false};
};

// Nothing when the object is missing or has no transform properties
// (volumes, bodies, decals, ... are not manipulable yet).
[[nodiscard]] std::optional<ManipulatorTarget>
ResolveManipulatorTarget(
    const scene::ObjectStore& objects,
    scene::ObjectId object);

[[nodiscard]] bool Supports(
    const ManipulatorTarget& target,
    ManipulatorTool tool) noexcept;

// Scale always acts on the object's own axes, whatever the toolbar says.
[[nodiscard]] ManipulatorSpace EffectiveSpace(
    ManipulatorTool tool,
    ManipulatorSpace requested) noexcept;

// Unit direction of a handle axis in the property frame. Uniform has no
// direction and returns the zero vector.
[[nodiscard]] math::Double3 ManipulatorAxisDirection(
    const ManipulatorTarget& target,
    ManipulatorTool tool,
    ManipulatorSpace space,
    ManipulatorAxis axis) noexcept;

// --- pure math, exposed for tests ---------------------------------------

[[nodiscard]] math::Double3x3 EulerDegreesToRotation(
    const math::Double3& eulerDegrees) noexcept;
[[nodiscard]] math::Double3 RotationToEulerDegrees(
    const math::Double3x3& rotation) noexcept;
[[nodiscard]] math::Double3x3 RotationAboutAxis(
    const math::Double3& unitAxis,
    f64 radians) noexcept;

// Parameter t of the point on the line origin + t * axis closest to the ray.
// Nothing when the ray is (nearly) parallel to the axis.
[[nodiscard]] std::optional<f64> ClosestAxisParameter(
    const ManipulatorRay& ray,
    const math::Double3& axisOrigin,
    const math::Double3& unitAxis) noexcept;

[[nodiscard]] std::optional<math::Double3> IntersectRayPlane(
    const ManipulatorRay& ray,
    const math::Double3& planePoint,
    const math::Double3& planeNormal) noexcept;

// One drag of one handle. Begin opens a command transaction, Update writes
// the absolute result for the current pointer (never incremental, so there is
// no drift), Commit makes the whole drag ONE undo step and Cancel restores
// the object exactly.
class ViewportManipulator
{
public:
    ViewportManipulator(
        scene::ObjectStore& objects,
        commands::CommandService& commands) noexcept;
    ~ViewportManipulator();

    ViewportManipulator(const ViewportManipulator&) = delete;
    ViewportManipulator& operator=(const ViewportManipulator&) = delete;

    // False when the tool/axis combination is unsupported for the target or
    // the grab ray is degenerate for it (nothing is opened then). Throws
    // std::logic_error when a drag or another transaction is already active.
    [[nodiscard]] bool Begin(
        const ManipulatorTarget& target,
        ManipulatorTool tool,
        ManipulatorSpace space,
        ManipulatorAxis axis,
        const ManipulatorRay& grabRay);

    void Update(
        const ManipulatorRay& ray,
        const ManipulatorSnap& snap);

    void Commit();
    void Cancel() noexcept;

    // Headless equivalent of one finished drag (RPC/MCP): applies `amount`
    // along `axis` as ONE undoable step and returns the summary. `amount` is
    // meters for Translate, degrees for Rotate and a positive factor for
    // Scale. Throws std::invalid_argument for an unsupported tool/axis/amount
    // and std::logic_error inside another transaction; a failed write is
    // rolled back.
    std::string ApplyDelta(
        const ManipulatorTarget& target,
        ManipulatorTool tool,
        ManipulatorSpace space,
        ManipulatorAxis axis,
        f64 amount);

    [[nodiscard]] bool Active() const noexcept;
    [[nodiscard]] std::optional<scene::ObjectId> Object() const noexcept;

    // Short readout of the current drag ("Move X +1.50 m").
    [[nodiscard]] const std::string& Summary() const noexcept;

private:
    void Clear() noexcept;
    void CaptureStartValues(
        const ManipulatorTarget& target,
        ManipulatorTool tool);
    // Writes the absolute result for `amount` (meters, degrees or factor)
    // measured from the captured start values.
    void WriteAmount(f64 amount);

    scene::ObjectStore& objects_;
    commands::CommandService& commands_;

    bool active_{false};
    ManipulatorTarget target_{};
    ManipulatorTool tool_{ManipulatorTool::Translate};
    ManipulatorAxis axis_{ManipulatorAxis::X};
    math::Double3 axisDirection_{};
    math::Double3 startPosition_{};
    math::Double3 startEuler_{};
    math::Double3 startSize_{};
    f64 startRadius_{0.0};
    bool sphereProxy_{false};
    math::Double3 startDirection_{};
    f64 startParameter_{0.0};
    math::Double3 startVector_{};
    math::Double3 planeNormal_{};
    f64 startDistance_{0.0};
    std::string summary_;
};
} // namespace orbit::editor_model

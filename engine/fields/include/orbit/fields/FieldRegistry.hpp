#pragma once

#include <orbit/core/StrongId.hpp>
#include <orbit/frames/FrameGraph.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/universe/BodyRegistry.hpp>

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace orbit::fields
{
struct FieldIdTag;
using FieldId = core::StrongId<FieldIdTag>;

enum class FieldValueKind : u8
{
    Scalar,
    Vector,
    Categorical,
    Material,
    Tensor
};

enum class FieldDomain : u8
{
    Surface,
    Volume,
    AtmosphericShell,
    LocalRegion,
    BodyInterior,
    FreeSpace
};

enum class FieldResidency : u8
{
    Cpu,
    Gpu,
    Hybrid
};

enum class FieldResolutionMode : u8
{
    Native,
    FixedSpacing,
    AdaptiveLod
};

struct FieldResolutionPolicy
{
    FieldResolutionMode mode{
        FieldResolutionMode::Native};
    f64 nominalSpacingMeters{0.0};
};

struct SurfaceFieldLocation
{
    universe::BodyId body{};
    math::Double3 unitDirection{
        0.0,
        1.0,
        0.0
    };
    f64 footprintMeters{1.0};
};

struct VolumeFieldLocation
{
    universe::BodyId body{};
    math::Double3 bodyLocalMeters{};
    f64 footprintMeters{1.0};
};

struct FreeSpaceFieldLocation
{
    frames::FramePoint point{};
    f64 footprintMeters{1.0};
};

using FieldLocation =
    std::variant<
        SurfaceFieldLocation,
        VolumeFieldLocation,
        FreeSpaceFieldLocation>;

struct CategoricalFieldValue
{
    u32 category{0};
};

struct MaterialFieldValue
{
    // Physical/material classes plus blend weights. These are semantic
    // class IDs, not renderer material asset handles.
    std::array<u32, 4> classes{};
    std::array<f32, 4> weights{};
};

struct TensorFieldValue
{
    std::array<f64, 9> components{};
};

using FieldValue =
    std::variant<
        f64,
        math::Double3,
        CategoricalFieldValue,
        MaterialFieldValue,
        TensorFieldValue>;

struct FieldDescriptor
{
    FieldId id{};
    std::optional<universe::BodyId> ownerBody;
    std::string name;
    FieldValueKind valueKind{
        FieldValueKind::Scalar};
    FieldDomain domain{
        FieldDomain::Surface};
    std::string unit;
    FieldResidency residency{
        FieldResidency::Cpu};
    FieldResolutionPolicy resolution{};
};

using CpuFieldEvaluator =
    std::function<std::optional<FieldValue>(
        const FieldLocation&)>;

using FieldRevisionProvider =
    std::function<u64()>;

struct FieldRegistration
{
    FieldDescriptor descriptor;
    CpuFieldEvaluator cpuEvaluator;
    FieldRevisionProvider revision;
};

class FieldRegistry
{
public:
    [[nodiscard]] FieldId Register(
        FieldRegistration registration);

    void Add(
        FieldRegistration registration);

    [[nodiscard]] const FieldDescriptor*
    Find(FieldId id) const noexcept;

    [[nodiscard]] std::vector<FieldId>
    FieldsForBody(
        universe::BodyId body) const;

    [[nodiscard]] u64 Revision(
        FieldId id) const noexcept;

    [[nodiscard]] std::optional<FieldValue>
    TrySampleCpu(
        FieldId id,
        const FieldLocation& location) const;

private:
    struct FieldRecord
    {
        FieldDescriptor descriptor;
        CpuFieldEvaluator cpuEvaluator;
        FieldRevisionProvider revision;
    };

    std::unordered_map<FieldId, FieldRecord>
        fields_;
};
} // namespace orbit::fields

#pragma once

#include <orbit/studio_session/ViewportTargetRegistry.hpp>

#include <array>
#include <algorithm>
#include <string_view>

namespace orbit::studio_ui
{
enum class ViewportLayout : u8
{
    Single,
    VerticalSplit,
    HorizontalSplit,
    Quad
};

enum class GizmoTool : u8
{
    Select,
    Translate,
    Rotate,
    Scale
};

enum class GizmoSpace : u8
{
    World,
    Local
};

enum class GizmoPivot : u8
{
    Median,
    Active,
    Individual
};

enum class SnapLengthUnit : u8 { Millimeters, Centimeters, Meters, Kilometers, Inches, Feet };

[[nodiscard]] constexpr std::string_view SnapUnitSymbol(SnapLengthUnit unit) noexcept
{
    switch (unit)
    {
    case SnapLengthUnit::Millimeters: return "mm";
    case SnapLengthUnit::Centimeters: return "cm";
    case SnapLengthUnit::Meters: return "m";
    case SnapLengthUnit::Kilometers: return "km";
    case SnapLengthUnit::Inches: return "in";
    case SnapLengthUnit::Feet: return "ft";
    }
    return "m";
}

[[nodiscard]] constexpr f64 SnapUnitMeters(SnapLengthUnit unit) noexcept
{
    switch (unit)
    {
    case SnapLengthUnit::Millimeters: return 0.001;
    case SnapLengthUnit::Centimeters: return 0.01;
    case SnapLengthUnit::Meters: return 1.0;
    case SnapLengthUnit::Kilometers: return 1000.0;
    case SnapLengthUnit::Inches: return 0.0254;
    case SnapLengthUnit::Feet: return 0.3048;
    }
    return 1.0;
}

struct GizmoSettings
{
    GizmoTool tool{GizmoTool::Select};
    GizmoSpace space{GizmoSpace::World};
    GizmoPivot pivot{GizmoPivot::Median};
    bool translationSnap{false};
    f64 translationSnapMeters{1.0};
    bool rotationSnap{false};
    f64 rotationSnapDegrees{15.0};
    bool scaleSnap{false};
    f64 scaleSnapStep{0.1};
    bool surfaceSnap{false};
    SnapLengthUnit translationSnapUnit{SnapLengthUnit::Meters};
};

struct ViewportAuthoringState
{
    ViewportLayout layout{ViewportLayout::Single};
    std::array<studio_session::ViewportMode, 4> modes{
        studio_session::ViewportMode::Perspective,
        studio_session::ViewportMode::BodyMap,
        studio_session::ViewportMode::Debug,
        studio_session::ViewportMode::System
    };
    u8 activeSlot{0};
    GizmoSettings gizmo{};

    [[nodiscard]] constexpr u8 SlotCount() const noexcept
    {
        switch (layout)
        {
        case ViewportLayout::Single: return 1U;
        case ViewportLayout::VerticalSplit:
        case ViewportLayout::HorizontalSplit: return 2U;
        case ViewportLayout::Quad: return 4U;
        }
        return 1U;
    }

    void SetLayout(const ViewportLayout next) noexcept
    {
        layout = next;
        activeSlot = std::min<u8>(
            activeSlot,
            static_cast<u8>(SlotCount() - 1U));
    }

    void SetActiveSlot(const u8 slot) noexcept
    {
        activeSlot = std::min<u8>(
            slot,
            static_cast<u8>(SlotCount() - 1U));
    }
};
} // namespace orbit::studio_ui

#pragma once

#include <orbit/commands/CommandService.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/selection/SelectionService.hpp>
#include <orbit/time/SimulationTime.hpp>
#include <orbit/world_model/UniverseComposition.hpp>

#include <optional>
#include <string>
#include <vector>

namespace orbit::editor_model
{
enum class SystemViewObjectKind : u8
{
    ReferenceNode = 0,
    Body = 1
};

struct SystemViewItem
{
    scene::ObjectId object{};
    std::string name;
    SystemViewObjectKind kind{SystemViewObjectKind::Body};
    math::Double3 positionMeters{};
    f64 distanceFromSystemOriginMeters{0.0};
};

struct SystemViewTrajectorySample
{
    time::SimulationTime time{};
    math::Double3 positionMeters{};
};

struct AnalyticOrbitEdit
{
    f64 semiMajorAxisMeters{1.0};
    f64 periapsisDistanceMeters{1.0};
    f64 eccentricity{0.0};
    f64 inclinationDegrees{0.0};
    f64 ascendingNodeDegrees{0.0};
    f64 argumentPeriapsisDegrees{0.0};
    f64 meanAnomalyEpochDegrees{0.0};
    f64 gravitationalParameterM3PerS2{1.0};
};

struct OrbitManipulationTarget
{
    scene::ObjectId body{};
    scene::ObjectId capability{};
    AnalyticOrbitEdit values{};
};

class SystemViewModel
{
public:
    SystemViewModel(
        scene::ObjectStore& objects,
        world_model::UniverseComposition& universe,
        selection::SelectionService& selection,
        commands::CommandService& commands);

    [[nodiscard]] std::vector<scene::ObjectRecord>
    Systems() const;

    [[nodiscard]] std::optional<scene::ObjectRecord>
    SystemForSelection() const;

    [[nodiscard]] std::vector<SystemViewItem>
    Items(
        scene::ObjectId system,
        time::SimulationTime atTime) const;

    [[nodiscard]] std::vector<SystemViewTrajectorySample>
    SampleTrajectory(
        scene::ObjectId object,
        scene::ObjectId system,
        time::SimulationTime start,
        f64 durationSeconds,
        u32 segments) const;

    [[nodiscard]] std::optional<OrbitManipulationTarget>
    AnalyticOrbitTarget(scene::ObjectId object) const;

    void ApplyAnalyticOrbitEdit(
        scene::ObjectId capability,
        const AnalyticOrbitEdit& edit);

    void SetReferenceNodePosition(
        scene::ObjectId object,
        math::Double3 positionMeters);

    [[nodiscard]] f64 SuggestedTrajectoryDurationSeconds(
        scene::ObjectId object) const;

    void Select(scene::ObjectId object);

private:
    [[nodiscard]] bool DescendsFrom(
        scene::ObjectId object,
        scene::ObjectId ancestor) const;

    scene::ObjectStore& objects_;
    world_model::UniverseComposition& universe_;
    selection::SelectionService& selection_;
    commands::CommandService& commands_;
};
} // namespace orbit::editor_model

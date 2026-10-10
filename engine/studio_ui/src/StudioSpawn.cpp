#include <orbit/studio_ui/StudioSpawn.hpp>

#include <orbit/world_model/WorldSchemas.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <utility>
#include <variant>

namespace orbit::studio_ui
{
namespace
{
using rpc::Value;

constexpr i64 kInvalid = 1080;
constexpr i64 kFailed = 1081;
constexpr f64 kRadians = std::numbers::pi / 180.0;

template <typename T>
[[nodiscard]] T PropertyOr(
    const scene::ObjectStore& objects,
    const scene::ObjectId object,
    const schema::PropertyId property,
    const T& fallback)
{
    const auto value = objects.GetProperty(object, property);
    if (value.has_value())
    {
        if (const auto* typed = std::get_if<T>(&*value))
        {
            return *typed;
        }
    }
    return fallback;
}

void Gather(
    const scene::ObjectStore& objects,
    const scene::ObjectRecord& record,
    std::vector<StudioSpawnLocation>& result)
{
    if (record.type == world_model::kSpawnLocationType && record.parent.has_value())
    {
        result.push_back({
            .id = record.id,
            .body = *record.parent,
            .name = record.name,
            .enabled = PropertyOr<bool>(
                objects, record.id, world_model::kSpawnLocationEnabled, true),
            .positionMeters = PropertyOr<math::Double3>(
                objects, record.id, world_model::kSpawnLocationPositionMeters, {}),
            .eyeHeightMeters = PropertyOr<f64>(
                objects, record.id, world_model::kSpawnLocationEyeHeightMeters, 2.0),
            .yawDegrees = PropertyOr<f64>(
                objects, record.id, world_model::kSpawnLocationYawDegrees, 0.0),
            .pitchDegrees = PropertyOr<f64>(
                objects, record.id, world_model::kSpawnLocationPitchDegrees, 0.0)});
    }
    for (const auto& child : objects.Children(record.id))
    {
        Gather(objects, child, result);
    }
}

[[nodiscard]] Value ToRpc(const StudioSpawnLocation& spawn)
{
    return Value(Value::Object{
        {"id", spawn.id.ToString()},
        {"body", spawn.body.ToString()},
        {"name", spawn.name},
        {"enabled", spawn.enabled},
        {"position", Value(Value::Array{
            spawn.positionMeters.x, spawn.positionMeters.y, spawn.positionMeters.z})},
        {"eye_height_meters", spawn.eyeHeightMeters},
        {"yaw_degrees", spawn.yawDegrees},
        {"pitch_degrees", spawn.pitchDegrees}});
}

[[nodiscard]] Value::Object ObjectOrEmpty(const Value& params)
{
    if (params.IsNull())
    {
        return {};
    }
    if (!params.IsObject())
    {
        throw rpc::Error(-32602, "Params must be an object.");
    }
    return params.AsObject();
}

[[nodiscard]] std::optional<std::string> OptionalString(
    const Value::Object& object, const char* key)
{
    const auto found = object.find(key);
    if (found == object.end() || found->second.IsNull())
    {
        return std::nullopt;
    }
    if (!found->second.IsString())
    {
        throw rpc::Error(-32602, std::string(key) + " must be a string.");
    }
    return found->second.AsString();
}
} // namespace

std::vector<StudioSpawnLocation> ListSpawnLocations(const scene::ObjectStore& objects)
{
    std::vector<StudioSpawnLocation> result;
    for (const auto& root : objects.Roots())
    {
        Gather(objects, root, result);
    }
    return result;
}

std::optional<StudioSpawnLocation> FindProjectSpawn(const scene::ObjectStore& objects)
{
    for (auto& spawn : ListSpawnLocations(objects))
    {
        if (spawn.enabled)
        {
            return std::move(spawn);
        }
    }
    return std::nullopt;
}

bool GoToSpawn(
    StudioRenderViewSet& views,
    studio_session::StudioSession& session,
    const std::string_view viewId,
    const StudioSpawnLocation& spawn)
{
    auto current = views.ViewPose(viewId);
    if (!current.has_value() || current->targetObject != spawn.body.ToString())
    {
        // The active body follows the selection, so the body has to be
        // selected for the view to stay on it.
        const scene::ObjectId selected[] = {spawn.body};
        session.World().Selection().Set(selected);
        session.ActiveBody().Focus(spawn.body);
        return false;
    }
    if (!views.HasTerrainNavigation(viewId) &&
        (!views.FocusTerrainBody(viewId) || !views.HasTerrainNavigation(viewId)))
    {
        return false;
    }

    const f64 length = math::Length(spawn.positionMeters);
    if (!(length > 0.0) || !std::isfinite(length))
    {
        throw std::runtime_error(
            "Spawn Location '" + spawn.name + "' has no body-local position.");
    }

    const math::Double3 direction{
        spawn.positionMeters.x / length,
        spawn.positionMeters.y / length,
        spawn.positionMeters.z / length};
    if (!views.FocusTerrainDirection(viewId, direction))
    {
        return false;
    }
    views.SetViewportMode(viewId, studio_session::ViewportMode::Perspective);

    auto pose = views.ViewPose(viewId);
    if (!pose.has_value())
    {
        return false;
    }
    // FocusTerrainDirection leaves the camera at its double-click vantage;
    // bring it down to the authored eye height over the terrain.
    if (const auto readout = views.NavigationReadout(viewId); readout.has_value())
    {
        const f64 radius = math::Length(pose->observerMeters);
        if (radius > 0.0)
        {
            const f64 target = radius - readout->heightAboveTerrainMeters +
                std::max(0.0, spawn.eyeHeightMeters);
            pose->observerMeters = pose->observerMeters * (target / radius);
        }
    }
    pose->yawRadians = spawn.yawDegrees * kRadians;
    pose->pitchRadians = spawn.pitchDegrees * kRadians;
    return views.RestoreViewPose(viewId, *pose);
}

void RegisterStudioSpawnRpc(
    rpc::Dispatcher& dispatcher,
    StudioRenderViewSet& views,
    studio_session::StudioSession& session)
{
    dispatcher.Register(
        {
            .name = "spawn.list",
            .description =
                "Every Spawn Location object (id, body, name, enabled, "
                "body-local position, yaw/pitch degrees) and which one is the "
                "project spawn: the first enabled one, where the camera lands "
                "whenever the project opens.",
            .mutating = false
        },
        [&session](const Value&)
        {
            if (!session.World().HasWorld())
            {
                throw rpc::Error(kFailed, "No world is open.");
            }
            const auto& objects = session.World().Objects();
            Value::Array list;
            for (const auto& spawn : ListSpawnLocations(objects))
            {
                list.push_back(ToRpc(spawn));
            }
            const auto project = FindProjectSpawn(objects);
            return Value(Value::Object{
                {"spawns", Value(std::move(list))},
                {"project_spawn",
                 project.has_value() ? Value(project->id.ToString()) : Value()}});
        });

    dispatcher.Register(
        {
            .name = "spawn.go",
            .description =
                "Moves the camera to a Spawn Location (id optional: the "
                "project spawn when omitted) the way the project-open spawn "
                "does: on the terrain below its position, then its yaw and "
                "pitch. view_id defaults to studio.primary. Returns "
                "went=false while the view is still focusing the spawn body or its terrain is not "
                "ready (retry).",
            .mutating = true
        },
        [&views, &session](const Value& params)
        {
            if (!session.World().HasWorld())
            {
                throw rpc::Error(kFailed, "No world is open.");
            }
            const auto object = ObjectOrEmpty(params);
            const auto& objects = session.World().Objects();
            std::optional<StudioSpawnLocation> chosen;
            if (const auto id = OptionalString(object, "id"))
            {
                const auto parsed = scene::ObjectId::Parse(*id);
                if (!parsed.has_value())
                {
                    throw rpc::Error(kInvalid, "id is not an object id: " + *id);
                }
                for (auto& spawn : ListSpawnLocations(objects))
                {
                    if (spawn.id == *parsed)
                    {
                        chosen = std::move(spawn);
                    }
                }
                if (!chosen.has_value())
                {
                    throw rpc::Error(kInvalid, "Object " + *id + " is not a Spawn Location.");
                }
            }
            else
            {
                chosen = FindProjectSpawn(objects);
                if (!chosen.has_value())
                {
                    throw rpc::Error(kInvalid, "The world has no enabled Spawn Location.");
                }
            }
            const std::string viewId =
                OptionalString(object, "view_id").value_or("studio.primary");
            try
            {
                const bool went = GoToSpawn(views, session, viewId, *chosen);
                return Value(Value::Object{
                    {"went", went}, {"spawn", ToRpc(*chosen)}});
            }
            catch (const std::exception& exception)
            {
                throw rpc::Error(kFailed, exception.what());
            }
        });

    dispatcher.Register(
        {
            .name = "spawn.set_here",
            .description =
                "Makes the camera's current spot the project spawn: updates "
                "the project spawn (or creates a Spawn Location under the "
                "view's target body) with the camera's body-local position, "
                "yaw and pitch. Optional name, view_id. One undoable edit.",
            .mutating = true
        },
        [&views, &session](const Value& params)
        {
            if (!session.World().HasWorld())
            {
                throw rpc::Error(kFailed, "No world is open.");
            }
            const auto object = ObjectOrEmpty(params);
            const std::string viewId =
                OptionalString(object, "view_id").value_or("studio.primary");
            const auto pose = views.ViewPose(viewId);
            if (!pose.has_value() || !pose->hasCamera)
            {
                throw rpc::Error(kInvalid, "View '" + viewId + "' has no camera pose yet.");
            }
            const auto body = scene::ObjectId::Parse(pose->targetObject);
            if (!body.has_value())
            {
                throw rpc::Error(kInvalid, "The view's target body is not an object.");
            }

            auto& world = session.World();
            auto& commands = world.Commands();
            const auto spawn = FindProjectSpawn(world.Objects());
            commands.BeginTransaction("Set Spawn Location");
            try
            {
                scene::ObjectId id{};
                if (spawn.has_value() && spawn->body == *body)
                {
                    id = spawn->id;
                }
                else
                {
                    id = commands.CreateObject(
                        world_model::kSpawnLocationType,
                        OptionalString(object, "name").value_or("Spawn Location"),
                        *body);
                }
                commands.SetProperty(
                    id, world_model::kSpawnLocationPositionMeters, pose->observerMeters);
                if (const auto readout = views.NavigationReadout(viewId);
                    readout.has_value())
                {
                    commands.SetProperty(
                        id, world_model::kSpawnLocationEyeHeightMeters,
                        std::max(0.0, readout->heightAboveTerrainMeters));
                }
                commands.SetProperty(
                    id, world_model::kSpawnLocationYawDegrees,
                    pose->yawRadians / kRadians);
                commands.SetProperty(
                    id, world_model::kSpawnLocationPitchDegrees,
                    std::clamp(pose->pitchRadians / kRadians, -89.0, 89.0));
                commands.CommitTransaction();
                for (const auto& listed : ListSpawnLocations(world.Objects()))
                {
                    if (listed.id == id)
                    {
                        return ToRpc(listed);
                    }
                }
                throw rpc::Error(kFailed, "The Spawn Location was not stored.");
            }
            catch (const rpc::Error&)
            {
                commands.RollbackTransaction();
                throw;
            }
            catch (const std::exception& exception)
            {
                commands.RollbackTransaction();
                throw rpc::Error(kFailed, exception.what());
            }
        });
}
} // namespace orbit::studio_ui

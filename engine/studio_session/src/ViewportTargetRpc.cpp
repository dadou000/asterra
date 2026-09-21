#include <orbit/studio_session/ViewportTargetRpc.hpp>

#include <stdexcept>
#include <string>
#include <utility>

namespace orbit::studio_session
{
namespace
{
[[nodiscard]] const rpc::Value::Object& RequireObject(
    const rpc::Value& params)
{
    if (!params.IsObject())
    {
        throw rpc::Error(-32602, "Params must be an object.");
    }

    return params.AsObject();
}

[[nodiscard]] std::string RequireString(
    const rpc::Value::Object& object,
    const std::string_view key)
{
    const auto found = object.find(key);

    if (found == object.end() ||
        !found->second.IsString() ||
        found->second.AsString().empty())
    {
        throw rpc::Error(
            -32602,
            std::string(key) + " must be a non-empty string.");
    }

    return found->second.AsString();
}

[[nodiscard]] bool OptionalBool(
    const rpc::Value::Object& object,
    const std::string_view key,
    const bool fallback)
{
    const auto found = object.find(key);

    if (found == object.end())
    {
        return fallback;
    }

    if (!found->second.IsBool())
    {
        throw rpc::Error(
            -32602,
            std::string(key) + " must be a boolean.");
    }

    return found->second.AsBool();
}

[[nodiscard]] ViewportMode ParseMode(
    const std::string_view text)
{
    if (text == "perspective")
    {
        return ViewportMode::Perspective;
    }
    if (text == "body_map")
    {
        return ViewportMode::BodyMap;
    }
    if (text == "debug")
    {
        return ViewportMode::Debug;
    }
    if (text == "system")
    {
        return ViewportMode::System;
    }

    throw rpc::Error(
        -32602,
        "Viewport mode must be perspective, body_map, debug, or system.");
}

[[nodiscard]] const char* ModeName(
    const ViewportMode mode) noexcept
{
    switch (mode)
    {
    case ViewportMode::Perspective:
        return "perspective";
    case ViewportMode::BodyMap:
        return "body_map";
    case ViewportMode::Debug:
        return "debug";
    case ViewportMode::System:
        return "system";
    }

    return "perspective";
}

[[nodiscard]] rpc::Value TargetToRpc(
    const editor_session::ActiveBodyTarget& target)
{
    return rpc::Value(
        rpc::Value::Object{
            {"object", target.semanticObject.ToString()},
            {"body", target.body.ToString()},
            {"frame", target.frame.ToString()},
            {"name", target.name},
            {"reference_radius_meters", target.referenceRadiusMeters},
            {
                "session_generation",
                static_cast<i64>(target.sessionGeneration)
            },
            {
                "universe_generation",
                static_cast<i64>(target.universeGeneration)
            },
            {
                "source_revision",
                static_cast<i64>(target.sourceRevision)
            }
        });
}

[[nodiscard]] rpc::Value ViewToRpc(
    const ViewportTargetState& view)
{
    rpc::Value::Object result{
        {"id", view.id},
        {"mode", ModeName(view.mode)},
        {"follow_active_body", view.followActiveBody}
    };

    result.emplace(
        "pinned_object",
        view.pinnedSemanticObject.has_value()
            ? rpc::Value(
                  view.pinnedSemanticObject->ToString())
            : rpc::Value{});

    result.emplace(
        "target",
        view.target.has_value()
            ? TargetToRpc(*view.target)
            : rpc::Value{});

    return rpc::Value(std::move(result));
}

[[nodiscard]] scene::ObjectId RequireObjectId(
    const rpc::Value::Object& object,
    const std::string_view key)
{
    const auto parsed =
        scene::ObjectId::Parse(
            RequireString(object, key));

    if (!parsed.has_value())
    {
        throw rpc::Error(
            -32602,
            std::string(key) + " must be a valid object ID.");
    }

    return *parsed;
}
} // namespace

void RegisterViewportTargetRpc(
    rpc::Dispatcher& dispatcher,
    ViewportTargetRegistry& registry)
{
    dispatcher.Register(
        {
            .name = "view.list",
            .description =
                "Returns independent Studio viewport body/frame targets.",
            .mutating = false
        },
        [&registry](const rpc::Value&)
        {
            rpc::Value::Array result;

            for (const auto& view : registry.Catalog())
            {
                result.push_back(ViewToRpc(view));
            }

            return rpc::Value(std::move(result));
        });

    dispatcher.Register(
        {
            .name = "view.register",
            .description =
                "Registers a Studio viewport target slot.",
            .mutating = true
        },
        [&registry](const rpc::Value& params)
        {
            const auto& values = RequireObject(params);
            ViewportMode mode = ViewportMode::Perspective;

            if (const auto found = values.find("mode");
                found != values.end())
            {
                if (!found->second.IsString())
                {
                    throw rpc::Error(
                        -32602,
                        "mode must be a string.");
                }

                mode = ParseMode(found->second.AsString());
            }

            try
            {
                registry.Register(
                    RequireString(values, "id"),
                    mode,
                    OptionalBool(
                        values,
                        "follow_active_body",
                        true));
            }
            catch (const rpc::Error&)
            {
                throw;
            }
            catch (const std::exception& exception)
            {
                throw rpc::Error(1030, exception.what());
            }

            const auto* view =
                registry.Find(RequireString(values, "id"));
            return ViewToRpc(*view);
        });

    dispatcher.Register(
        {
            .name = "view.unregister",
            .description =
                "Removes a Studio viewport target slot.",
            .mutating = true
        },
        [&registry](const rpc::Value& params)
        {
            const auto& values = RequireObject(params);
            const std::string id = RequireString(values, "id");

            if (!registry.Unregister(id))
            {
                throw rpc::Error(1031, "Viewport target ID was not found.");
            }

            return rpc::Value(
                rpc::Value::Object{{"ok", true}});
        });

    dispatcher.Register(
        {
            .name = "view.set_mode",
            .description =
                "Changes a Studio viewport presentation mode.",
            .mutating = true
        },
        [&registry](const rpc::Value& params)
        {
            const auto& values = RequireObject(params);

            try
            {
                const std::string id = RequireString(values, "id");
                registry.SetMode(
                    id,
                    ParseMode(RequireString(values, "mode")));
                return ViewToRpc(*registry.Find(id));
            }
            catch (const rpc::Error&)
            {
                throw;
            }
            catch (const std::exception& exception)
            {
                throw rpc::Error(1031, exception.what());
            }
        });

    dispatcher.Register(
        {
            .name = "view.follow_active",
            .description =
                "Makes a Studio viewport follow the shared active body.",
            .mutating = true
        },
        [&registry](const rpc::Value& params)
        {
            const auto& values = RequireObject(params);

            try
            {
                const std::string id = RequireString(values, "id");
                registry.FollowActiveBody(id);
                static_cast<void>(registry.Refresh());
                return ViewToRpc(*registry.Find(id));
            }
            catch (const rpc::Error&)
            {
                throw;
            }
            catch (const std::exception& exception)
            {
                throw rpc::Error(1031, exception.what());
            }
        });

    dispatcher.Register(
        {
            .name = "view.pin",
            .description =
                "Pins a Studio viewport to a semantic celestial body.",
            .mutating = true
        },
        [&registry](const rpc::Value& params)
        {
            const auto& values = RequireObject(params);

            try
            {
                const std::string id = RequireString(values, "id");
                registry.PinToObject(
                    id,
                    RequireObjectId(values, "object"));
                return ViewToRpc(*registry.Find(id));
            }
            catch (const rpc::Error&)
            {
                throw;
            }
            catch (const std::exception& exception)
            {
                throw rpc::Error(1032, exception.what());
            }
        });
}
} // namespace orbit::studio_session

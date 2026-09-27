#include <orbit/studio_ui/StudioRenderViewRpc.hpp>

#include <stdexcept>
#include <string>

namespace orbit::studio_ui
{
namespace
{
using rpc::Value;

constexpr i64 kInvalid = 1070;
constexpr i64 kFailed = 1071;

[[nodiscard]] const Value::Object& RequireObject(
    const Value& params)
{
    if (!params.IsObject())
    {
        throw rpc::Error(-32602, "Params must be an object.");
    }

    return params.AsObject();
}

[[nodiscard]] std::string RequireString(
    const Value::Object& object,
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

[[nodiscard]] lighting::SurfaceDebugMode ParseSurfaceDebugMode(
    const std::string_view text)
{
    if (text == "lit")
    {
        return lighting::SurfaceDebugMode::Lit;
    }
    if (text == "base_color_roughness")
    {
        return lighting::SurfaceDebugMode::BaseColorRoughness;
    }
    if (text == "normal_metallic")
    {
        return lighting::SurfaceDebugMode::NormalMetallic;
    }
    if (text == "emission_metadata")
    {
        return lighting::SurfaceDebugMode::EmissionMetadata;
    }

    throw rpc::Error(
        -32602,
        "surface_debug_mode must be lit, base_color_roughness, "
        "normal_metallic, or emission_metadata.");
}

[[nodiscard]] const char* SurfaceDebugModeName(
    const lighting::SurfaceDebugMode mode) noexcept
{
    switch (mode)
    {
    case lighting::SurfaceDebugMode::Lit:
        return "lit";
    case lighting::SurfaceDebugMode::BaseColorRoughness:
        return "base_color_roughness";
    case lighting::SurfaceDebugMode::NormalMetallic:
        return "normal_metallic";
    case lighting::SurfaceDebugMode::EmissionMetadata:
        return "emission_metadata";
    }

    return "lit";
}

[[nodiscard]] Value ToRpc(
    const std::string& id,
    const lighting::SurfaceDebugMode mode)
{
    return Value(
        Value::Object{
            {"id", id},
            {"surface_debug_mode", SurfaceDebugModeName(mode)}
        });
}
} // namespace

void RegisterStudioRenderViewRpc(
    rpc::Dispatcher& dispatcher,
    StudioRenderViewSet& views)
{
    dispatcher.Register(
        {
            .name = "view.surface_debug_get",
            .description =
                "Returns the GBuffer debug-view channel a Studio "
                "RenderView is currently showing "
                "(lit | base_color_roughness | normal_metallic | "
                "emission_metadata).",
            .mutating = false
        },
        [&views](const Value& params)
        {
            const auto& values = RequireObject(params);
            const std::string id = RequireString(values, "id");

            try
            {
                return ToRpc(id, views.SurfaceDebugMode(id));
            }
            catch (const rpc::Error&)
            {
                throw;
            }
            catch (const std::exception& exception)
            {
                throw rpc::Error(kInvalid, exception.what());
            }
        });

    dispatcher.Register(
        {
            .name = "view.surface_debug_set",
            .description =
                "Switches a Studio RenderView's GBuffer debug-view channel "
                "(lit | base_color_roughness | normal_metallic | "
                "emission_metadata). The same operation the Debug tab and "
                "each Viewport panel's 'Surface View' buttons drive.",
            .mutating = true
        },
        [&views](const Value& params)
        {
            const auto& values = RequireObject(params);
            const std::string id = RequireString(values, "id");
            const auto mode =
                ParseSurfaceDebugMode(
                    RequireString(values, "surface_debug_mode"));

            try
            {
                views.SetSurfaceDebugMode(id, mode);
                return ToRpc(id, views.SurfaceDebugMode(id));
            }
            catch (const rpc::Error&)
            {
                throw;
            }
            catch (const std::exception& exception)
            {
                throw rpc::Error(kFailed, exception.what());
            }
        });
}
} // namespace orbit::studio_ui

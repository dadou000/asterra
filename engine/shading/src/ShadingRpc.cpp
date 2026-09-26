#include <orbit/shading/ShadingRpc.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace orbit::shading
{
namespace
{
using rpc::Value;

constexpr i64 kInvalid = 1050;
constexpr i64 kFailed = 1051;

const Value::Object& Params(const Value& params)
{
    static const Value::Object kEmpty;

    if (params.IsNull())
    {
        return kEmpty;
    }
    if (!params.IsObject())
    {
        throw rpc::Error(-32602, "Params must be an object.");
    }
    return params.AsObject();
}

std::string RequireString(const Value::Object& object, const std::string_view key)
{
    const auto found = object.find(key);
    if (found == object.end() || !found->second.IsString() ||
        found->second.AsString().empty())
    {
        throw rpc::Error(
            -32602, std::string(key) + " must be a non-empty string.");
    }
    return found->second.AsString();
}

std::optional<std::string> OptionalString(
    const Value::Object& object, const std::string_view key)
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

std::optional<f64> OptionalNumber(
    const Value::Object& object, const std::string_view key)
{
    const auto found = object.find(key);
    if (found == object.end() || found->second.IsNull())
    {
        return std::nullopt;
    }
    if (!found->second.IsNumber())
    {
        throw rpc::Error(-32602, std::string(key) + " must be a number.");
    }
    return found->second.AsNumber();
}

std::optional<bool> OptionalBool(
    const Value::Object& object, const std::string_view key)
{
    const auto found = object.find(key);
    if (found == object.end() || found->second.IsNull())
    {
        return std::nullopt;
    }
    if (!found->second.IsBool())
    {
        throw rpc::Error(-32602, std::string(key) + " must be a boolean.");
    }
    return found->second.AsBool();
}

// Maps workspace/content failures onto stable RPC errors.
template <typename Fn>
auto Guard(Fn&& fn) -> decltype(fn())
{
    try
    {
        return fn();
    }
    catch (const rpc::Error&)
    {
        throw;
    }
    catch (const std::invalid_argument& exception)
    {
        throw rpc::Error(kInvalid, exception.what());
    }
    catch (const std::exception& exception)
    {
        throw rpc::Error(kFailed, exception.what());
    }
}

Value Path(const std::filesystem::path& path)
{
    return Value(path.generic_string());
}

Value Numbers(const std::array<f64, 4>& values, const u32 count)
{
    Value::Array array;
    for (u32 index = 0U; index < count; ++index)
    {
        array.emplace_back(values[index]);
    }
    return Value(std::move(array));
}

Value NodeToValue(const TreeNode& node)
{
    Value::Object object{
        {"name", node.name},
        {"path", Path(node.path)},
        {"type", node.folder ? "folder" : "asset"}};

    if (node.kind.has_value())
    {
        object.emplace("kind", std::string(content::AssetKindName(*node.kind)));
        object.emplace("previewable", node.previewable);
    }

    if (node.folder)
    {
        Value::Array children;
        for (const auto& child : node.children)
        {
            children.push_back(NodeToValue(child));
        }
        object.emplace("children", Value(std::move(children)));
    }

    return Value(std::move(object));
}

Value PreviewToValue(const PreviewState& state)
{
    return Value(Value::Object{
        {"shape", std::string(ShapeKey(state.shape))},
        {"lighting", std::string(LightingKey(state.lighting))},
        {"background", std::string(BackgroundKey(state.background))},
        {"sun_azimuth_degrees", static_cast<f64>(state.sunAzimuthDegrees)},
        {"sun_elevation_degrees", static_cast<f64>(state.sunElevationDegrees)},
        {"exposure", static_cast<f64>(state.exposure)},
        {"model_yaw_degrees", static_cast<f64>(state.modelYawDegrees)},
        {"animate", state.animate},
        {"camera",
         Value::Object{
             {"yaw_degrees",
              static_cast<f64>(state.camera.yawRadians) * 180.0 /
                  std::numbers::pi},
             {"pitch_degrees",
              static_cast<f64>(state.camera.pitchRadians) * 180.0 /
                  std::numbers::pi},
             {"distance", static_cast<f64>(state.camera.distance)},
             {"fov_degrees",
              static_cast<f64>(state.camera.verticalFovRadians) * 180.0 /
                  std::numbers::pi}}}});
}

Value ParametersToValue(const ShadingWorkspace& workspace)
{
    Value::Array parameters;

    for (const auto& parameter : workspace.Parameters())
    {
        const auto& declaration = parameter.declaration;

        Value::Object object{
            {"name", declaration.name},
            {"type",
             declaration.isColor
                 ? std::string("color3")
                 : (declaration.components == 1U
                        ? std::string("float")
                        : "float" + std::to_string(declaration.components))},
            {"components", static_cast<i64>(declaration.components)},
            {"default", Numbers(declaration.defaults, declaration.components)},
            {"value", Numbers(parameter.value, declaration.components)},
            {"overridden", parameter.overridden}};

        if (declaration.hasRange)
        {
            object.emplace("min", declaration.minimum);
            object.emplace("max", declaration.maximum);
        }

        parameters.emplace_back(std::move(object));
    }

    return Value(std::move(parameters));
}

Value StatusToValue(const ShadingWorkspace& workspace)
{
    const ShaderStatus& status = workspace.Status();

    return Value(Value::Object{
        {"selected", Path(workspace.Selected())},
        {"shader", Path(status.shader)},
        {"material", Path(status.material)},
        {"compiled", status.compiled},
        {"diagnostics", status.diagnostics},
        {"program_revision", static_cast<i64>(status.programRevision)},
        {"compile_ms", status.compileMilliseconds},
        {"dirty", status.dirty},
        {"changed_on_disk", status.changedOnDisk},
        {"live_compile", workspace.LiveCompile()},
        {"parameters", ParametersToValue(workspace)},
        {"preview", PreviewToValue(workspace.Preview())}});
}

std::vector<f64> ParameterNumbers(const Value& value)
{
    std::vector<f64> numbers;

    if (value.IsNumber())
    {
        numbers.push_back(value.AsNumber());
    }
    else if (value.IsArray())
    {
        for (const auto& element : value.AsArray())
        {
            if (!element.IsNumber())
            {
                throw rpc::Error(-32602, "value must contain only numbers.");
            }
            numbers.push_back(element.AsNumber());
        }
    }
    else
    {
        throw rpc::Error(-32602, "value must be a number or an array of numbers.");
    }

    return numbers;
}

const std::vector<std::string_view>& MethodNames()
{
    static const std::vector<std::string_view> names{
        "shading.options",       "shading.tree",
        "shading.folder_create", "shading.rename",
        "shading.move",          "shading.trash",
        "shading.shader_create", "shading.material_create",
        "shading.select",        "shading.source_read",
        "shading.source_write",  "shading.status",
        "shading.param_set",     "shading.param_reset",
        "shading.preview_get",   "shading.preview_set",
        "shading.recompile",     "shading.screenshot"};
    return names;
}
} // namespace

std::vector<std::string> ShadingRpcMethodNames()
{
    std::vector<std::string> result;
    for (const auto name : MethodNames())
    {
        result.emplace_back(name);
    }
    return result;
}

void RegisterShadingRpc(
    rpc::Dispatcher& dispatcher,
    ShadingWorkspace& workspace,
    ShadingRpcHooks hooks)
{
    const auto add = [&](std::string name,
                         std::string description,
                         const bool mutating,
                         rpc::Dispatcher::MethodHandler handler)
    {
        dispatcher.Register(
            {.name = std::move(name),
             .description = std::move(description),
             .mutating = mutating},
            std::move(handler));
    };

    add("shading.options",
        "Lists the preview shapes, lighting presets, backgrounds and shader templates the Shading tab offers.",
        false,
        [](const Value&)
        {
            const auto entries = [](std::span<const EnumEntry> table)
            {
                Value::Array array;
                for (const auto& entry : table)
                {
                    array.emplace_back(Value::Object{
                        {"key", std::string(entry.key)},
                        {"label", std::string(entry.label)}});
                }
                return Value(std::move(array));
            };

            Value::Array templates;
            for (const auto name : ShaderTemplateNames())
            {
                templates.emplace_back(std::string(name));
            }

            return Value(Value::Object{
                {"shapes", entries(Shapes())},
                {"lighting", entries(LightingPresets())},
                {"backgrounds", entries(Backgrounds())},
                {"shader_templates", Value(std::move(templates))},
                {"max_parameter_floats", static_cast<i64>(kMaxParameterFloats)}});
        });

    add("shading.tree",
        "Returns the Content tree (folders and assets) the Shading tab shows, nested.",
        false,
        [&workspace](const Value&)
        {
            return Guard([&] { return NodeToValue(workspace.Tree()); });
        });

    add("shading.folder_create",
        "Creates a folder under Content. Paths may be project-relative (Content/Shading) or content-relative (Shading).",
        true,
        [&workspace](const Value& params)
        {
            const auto& values = Params(params);
            const auto path = RequireString(values, "path");
            return Guard(
                [&]
                {
                    workspace.CreateFolder(path);
                    return Value(Value::Object{{"path", path}});
                });
        });

    add("shading.rename",
        "Renames a file or folder in place. `name` is a single path component.",
        true,
        [&workspace](const Value& params)
        {
            const auto& values = Params(params);
            const auto path = RequireString(values, "path");
            const auto name = RequireString(values, "name");
            return Guard(
                [&]
                {
                    return Value(Value::Object{
                        {"path", Path(workspace.Rename(path, name))}});
                });
        });

    add("shading.move",
        "Moves a file or folder into an existing folder. Refuses collisions and moving a folder into itself.",
        true,
        [&workspace](const Value& params)
        {
            const auto& values = Params(params);
            const auto path = RequireString(values, "path");
            const auto folder = RequireString(values, "folder");
            return Guard(
                [&]
                {
                    return Value(Value::Object{
                        {"path", Path(workspace.Move(path, folder))}});
                });
        });

    add("shading.trash",
        "Reversibly removes a file or folder: it moves to .orbit/Trash inside the project.",
        true,
        [&workspace](const Value& params)
        {
            const auto& values = Params(params);
            const auto path = RequireString(values, "path");
            return Guard(
                [&]
                {
                    return Value(Value::Object{
                        {"trashed_to", Path(workspace.Trash(path))}});
                });
        });

    add("shading.shader_create",
        "Creates <folder>/<name>.shade.hlsl from a template (see shading.options) and opens it.",
        true,
        [&workspace](const Value& params)
        {
            const auto& values = Params(params);
            const auto folder = RequireString(values, "folder");
            const auto name = RequireString(values, "name");
            const auto templateName =
                OptionalString(values, "template").value_or("lit");
            return Guard(
                [&]
                {
                    const auto path =
                        workspace.CreateShader(folder, name, templateName);
                    workspace.Select(path);
                    return StatusToValue(workspace);
                });
        });

    add("shading.material_create",
        "Creates <folder>/<name>.orbitshadermaterial bound to a shader and opens it.",
        true,
        [&workspace](const Value& params)
        {
            const auto& values = Params(params);
            const auto folder = RequireString(values, "folder");
            const auto name = RequireString(values, "name");
            const auto shader = RequireString(values, "shader");
            return Guard(
                [&]
                {
                    const auto path =
                        workspace.CreateShaderMaterial(folder, name, shader);
                    workspace.Select(path);
                    return StatusToValue(workspace);
                });
        });

    add("shading.select",
        "Opens a shader or shader material in the Shading tab (loads and compiles it). Null clears the selection.",
        true,
        [&workspace](const Value& params)
        {
            const auto& values = Params(params);
            const auto path = OptionalString(values, "path");
            return Guard(
                [&]
                {
                    if (path.has_value())
                    {
                        workspace.Select(*path);
                    }
                    else
                    {
                        workspace.ClearSelection();
                    }
                    return StatusToValue(workspace);
                });
        });

    add("shading.source_read",
        "Returns the text of a shader source file.",
        false,
        [&workspace](const Value& params)
        {
            const auto& values = Params(params);
            const auto path = RequireString(values, "path");
            return Guard(
                [&]
                {
                    return Value(Value::Object{
                        {"path", path},
                        {"text", workspace.ReadSource(path)}});
                });
        });

    add("shading.source_write",
        "Writes a *.shade.hlsl file (creating it if needed), opens it, and compiles it. The running Studio is not restarted; diagnostics are in the result.",
        true,
        [&workspace](const Value& params)
        {
            const auto& values = Params(params);
            const auto path = RequireString(values, "path");
            const auto found = values.find("text");
            if (found == values.end() || !found->second.IsString())
            {
                throw rpc::Error(-32602, "text must be a string.");
            }
            return Guard(
                [&]
                {
                    workspace.WriteSource(path, found->second.AsString());
                    return StatusToValue(workspace);
                });
        });

    add("shading.status",
        "Returns the current selection, compile status and diagnostics, declared parameters with values, and the preview settings.",
        false,
        [&workspace](const Value&) { return StatusToValue(workspace); });

    add("shading.param_set",
        "Sets a shader parameter by name (number or array). With a shader material open the override is saved into the material.",
        true,
        [&workspace](const Value& params)
        {
            const auto& values = Params(params);
            const auto name = RequireString(values, "name");
            const auto found = values.find("value");
            if (found == values.end())
            {
                throw rpc::Error(-32602, "value is required.");
            }
            const auto numbers = ParameterNumbers(found->second);
            return Guard(
                [&]
                {
                    workspace.SetParameter(name, numbers);
                    return StatusToValue(workspace);
                });
        });

    add("shading.param_reset",
        "Removes a parameter override so the shader's declared default applies.",
        true,
        [&workspace](const Value& params)
        {
            const auto& values = Params(params);
            const auto name = RequireString(values, "name");
            return Guard(
                [&]
                {
                    workspace.ResetParameter(name);
                    return StatusToValue(workspace);
                });
        });

    add("shading.preview_get",
        "Returns the preview settings: shape, lighting preset, background, sun, exposure, camera.",
        false,
        [&workspace](const Value&)
        { return PreviewToValue(workspace.Preview()); });

    add("shading.preview_set",
        "Changes preview settings. Omitted fields keep their value. See shading.options for valid shape/lighting/background keys.",
        true,
        [&workspace](const Value& params)
        {
            const auto& values = Params(params);
            return Guard(
                [&]
                {
                    PreviewState& state = workspace.Preview();
                    constexpr f32 kRadians = std::numbers::pi_v<f32> / 180.0F;

                    if (const auto key = OptionalString(values, "shape"))
                    {
                        const auto parsed = ParseShape(*key);
                        if (!parsed) throw std::invalid_argument("Unknown shape '" + *key + "'.");
                        state.shape = *parsed;
                    }
                    if (const auto key = OptionalString(values, "lighting"))
                    {
                        const auto parsed = ParseLightingPreset(*key);
                        if (!parsed) throw std::invalid_argument("Unknown lighting preset '" + *key + "'.");
                        state.lighting = *parsed;
                    }
                    if (const auto key = OptionalString(values, "background"))
                    {
                        const auto parsed = ParseBackground(*key);
                        if (!parsed) throw std::invalid_argument("Unknown background '" + *key + "'.");
                        state.background = *parsed;
                    }
                    if (const auto v = OptionalNumber(values, "sun_azimuth_degrees"))
                        state.sunAzimuthDegrees = static_cast<f32>(*v);
                    if (const auto v = OptionalNumber(values, "sun_elevation_degrees"))
                        state.sunElevationDegrees =
                            static_cast<f32>(std::clamp(*v, -89.0, 89.0));
                    if (const auto v = OptionalNumber(values, "exposure"))
                        state.exposure = static_cast<f32>(std::clamp(*v, 0.01, 64.0));
                    if (const auto v = OptionalNumber(values, "model_yaw_degrees"))
                        state.modelYawDegrees = static_cast<f32>(*v);
                    if (const auto v = OptionalBool(values, "animate"))
                        state.animate = *v;

                    if (const auto camera = values.find("camera");
                        camera != values.end() && camera->second.IsObject())
                    {
                        const auto& c = camera->second.AsObject();
                        if (const auto v = OptionalNumber(c, "yaw_degrees"))
                            state.camera.yawRadians = static_cast<f32>(*v) * kRadians;
                        if (const auto v = OptionalNumber(c, "pitch_degrees"))
                            state.camera.pitchRadians =
                                static_cast<f32>(std::clamp(*v, -85.0, 85.0)) * kRadians;
                        if (const auto v = OptionalNumber(c, "distance"))
                            state.camera.distance =
                                static_cast<f32>(std::clamp(*v, 1.2, 20.0));
                        if (const auto v = OptionalNumber(c, "fov_degrees"))
                            state.camera.verticalFovRadians =
                                static_cast<f32>(std::clamp(*v, 15.0, 110.0)) * kRadians;
                    }

                    if (const auto v = OptionalBool(values, "live_compile"))
                        workspace.SetLiveCompile(*v);

                    return PreviewToValue(state);
                });
        });

    add("shading.recompile",
        "Recompiles the open shader from the editor buffer and returns the status.",
        true,
        [&workspace](const Value&)
        {
            return Guard(
                [&]
                {
                    workspace.Recompile();
                    return StatusToValue(workspace);
                });
        });

    add("shading.screenshot",
        "Captures the Shading preview to a 32-bit BMP at `path`.",
        false,
        [hooks = std::move(hooks)](const Value& params)
        {
            const auto& values = Params(params);
            const auto path = RequireString(values, "path");

            if (!hooks.screenshot)
            {
                throw rpc::Error(kFailed, "No Shading preview target is attached.");
            }

            return Guard([&] { return hooks.screenshot(path); });
        });
}
} // namespace orbit::shading

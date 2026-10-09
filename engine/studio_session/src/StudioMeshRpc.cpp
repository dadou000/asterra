#include <orbit/studio_session/StudioMeshRpc.hpp>

#include <orbit/mesh_import/GltfImporter.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/world_model/PrimitiveBinding.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace orbit::studio_session
{
namespace
{
namespace fs = std::filesystem;

[[nodiscard]] const rpc::Value::Object& RequireObject(
    const rpc::Value& params)
{
    if (!params.IsObject())
    {
        throw rpc::Error(-32602, "Params must be an object.");
    }
    return params.AsObject();
}

[[nodiscard]] std::string OptionalString(
    const rpc::Value::Object& object,
    const std::string_view key)
{
    const auto found = object.find(key);
    if (found == object.end())
    {
        return {};
    }
    if (!found->second.IsString())
    {
        throw rpc::Error(
            -32602, std::string(key) + " must be a string.");
    }
    return found->second.AsString();
}

[[nodiscard]] std::string Lower(std::string text)
{
    std::transform(
        text.begin(), text.end(), text.begin(),
        [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

[[nodiscard]] std::optional<math::Double3> OptionalVector(
    const rpc::Value::Object& object,
    const std::string_view key,
    const bool uniformScalarAllowed = false)
{
    const auto found = object.find(key);
    if (found == object.end() || found->second.IsNull())
    {
        return std::nullopt;
    }
    if (uniformScalarAllowed && found->second.IsNumber())
    {
        const f64 value = found->second.AsNumber();
        return math::Double3{value, value, value};
    }
    if (!found->second.IsArray() || found->second.AsArray().size() != 3U)
    {
        throw rpc::Error(
            -32602, std::string(key) + " must be an array of three numbers.");
    }
    std::array<f64, 3> v{};
    for (std::size_t i = 0U; i < 3U; ++i)
    {
        const auto& component = found->second.AsArray()[i];
        if (!component.IsNumber())
        {
            throw rpc::Error(
                -32602, std::string(key) + " must contain only numbers.");
        }
        v[i] = component.AsNumber();
    }
    return math::Double3{v[0], v[1], v[2]};
}

[[nodiscard]] std::optional<f64> OptionalNumber(
    const rpc::Value::Object& object,
    const std::string_view key)
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

[[nodiscard]] world_model::PrimitiveShape ParseShape(const std::string& text)
{
    const std::string name = Lower(text);
    if (name == "box" || name == "cube") return world_model::PrimitiveShape::Box;
    if (name == "sphere") return world_model::PrimitiveShape::Sphere;
    if (name == "cylinder") return world_model::PrimitiveShape::Cylinder;
    if (name == "capsule") return world_model::PrimitiveShape::Capsule;
    if (name == "plane") return world_model::PrimitiveShape::Plane;
    throw rpc::Error(
        -32602,
        "shape must be box, sphere, cylinder, capsule or plane.");
}

[[nodiscard]] world_model::PrimitiveSurface ParseSurface(
    const std::string& text)
{
    const std::string name = Lower(text);
    if (name.empty() || name == "standard")
    {
        return world_model::PrimitiveSurface::Standard;
    }
    if (name == "mirror") return world_model::PrimitiveSurface::Mirror;
    if (name == "glass") return world_model::PrimitiveSurface::Glass;
    if (name == "emissive") return world_model::PrimitiveSurface::Emissive;
    throw rpc::Error(
        -32602, "surface must be standard, mirror, glass or emissive.");
}

// A folder-safe asset name: letters, digits, '-', '_' and spaces only.
[[nodiscard]] std::string SafeName(const std::string& requested)
{
    std::string name;
    for (const char c : requested)
    {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' ||
            c == '_' || c == ' ')
        {
            name.push_back(c);
        }
        else
        {
            name.push_back('_');
        }
    }
    while (!name.empty() && name.front() == ' ')
    {
        name.erase(name.begin());
    }
    return name;
}

// The model file to import: the path itself, or the single .glb/.gltf found
// in a directory (preferring .glb).
[[nodiscard]] fs::path ResolveSource(const fs::path& source)
{
    if (fs::is_regular_file(source))
    {
        return source;
    }
    if (!fs::is_directory(source))
    {
        throw rpc::Error(1050, "source is not a file or folder: " + source.string());
    }

    fs::path glb;
    fs::path gltf;
    for (const auto& entry : fs::recursive_directory_iterator(source))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }
        const std::string extension = Lower(entry.path().extension().string());
        if (extension == ".glb" && glb.empty())
        {
            glb = entry.path();
        }
        else if (extension == ".gltf" && gltf.empty())
        {
            gltf = entry.path();
        }
    }
    if (!glb.empty())
    {
        return glb;
    }
    if (!gltf.empty())
    {
        return gltf;
    }
    throw rpc::Error(1050, "no .glb or .gltf file found in " + source.string());
}

[[nodiscard]] rpc::Value::Array Vector3(const std::array<f64, 3>& v)
{
    return {rpc::Value(v[0]), rpc::Value(v[1]), rpc::Value(v[2])};
}
} // namespace

void RegisterStudioMeshRpc(
    rpc::Dispatcher& dispatcher,
    StudioSession& session)
{
    dispatcher.Register(
        {
            .name = "mesh.import",
            .description =
                "Imports a glTF 2.0 model into the open project: validates "
                "`source` (a .glb or .gltf file, or a folder containing one), "
                "copies it (for .gltf with its sibling files) into "
                "Content/Models/<name>/ and returns asset_path (project-"
                "relative, for a Static Mesh's Mesh Asset property), "
                "triangles, materials, textures, bounds_min_meters / "
                "bounds_max_meters and import warnings. `name` defaults to "
                "the source's file name; an existing folder gets a numeric "
                "suffix. Refuses files that need Draco/meshopt decoding.",
            .mutating = true
        },
        [&session](const rpc::Value& params)
        {
            const auto& values = RequireObject(params);
            const std::string sourceText = OptionalString(values, "source");
            if (sourceText.empty())
            {
                throw rpc::Error(-32602, "source must be a non-empty string.");
            }

            try
            {
                const fs::path model = ResolveSource(fs::path(sourceText));
                const std::string extension = Lower(model.extension().string());
                if (extension != ".glb" && extension != ".gltf")
                {
                    throw rpc::Error(
                        1050, "only .glb and .gltf models can be imported.");
                }

                // Validate before touching the project.
                const auto asset = mesh_import::ImportGltfFile(model);

                std::string name = SafeName(OptionalString(values, "name"));
                if (name.empty())
                {
                    name = SafeName(model.stem().string());
                }
                if (name.empty())
                {
                    name = "Model";
                }

                const fs::path root = session.World().Project().RootDirectory();
                const fs::path modelsRoot = root / "Content" / "Models";
                fs::path folder = modelsRoot / name;
                for (u32 suffix = 2U; fs::exists(folder); ++suffix)
                {
                    folder = modelsRoot / (name + "_" + std::to_string(suffix));
                }
                fs::create_directories(folder);

                fs::path destination = folder / model.filename();
                if (extension == ".glb")
                {
                    fs::copy_file(model, destination);
                }
                else
                {
                    // External buffers and images sit beside the .gltf.
                    for (const auto& entry :
                         fs::recursive_directory_iterator(model.parent_path()))
                    {
                        if (!entry.is_regular_file())
                        {
                            continue;
                        }
                        const fs::path relative =
                            fs::relative(entry.path(), model.parent_path());
                        const fs::path target = folder / relative;
                        fs::create_directories(target.parent_path());
                        fs::copy_file(entry.path(), target);
                    }
                }

                const std::string assetPath =
                    fs::relative(destination, root).generic_string();

                rpc::Value::Array warnings;
                for (const auto& warning : asset.warnings)
                {
                    warnings.emplace_back(warning);
                }

                return rpc::Value(rpc::Value::Object{
                    {"asset_path", assetPath},
                    {"triangles", static_cast<f64>(asset.TriangleCount())},
                    {"materials", static_cast<f64>(asset.materials.size())},
                    {"textures", static_cast<f64>(asset.textures.size())},
                    {"bounds_min_meters", rpc::Value(Vector3(asset.boundsMin))},
                    {"bounds_max_meters", rpc::Value(Vector3(asset.boundsMax))},
                    {"warnings", rpc::Value(std::move(warnings))}});
            }
            catch (const rpc::Error&)
            {
                throw;
            }
            catch (const mesh_import::MeshImportError& error)
            {
                throw rpc::Error(1050, error.what());
            }
            catch (const std::exception& error)
            {
                throw rpc::Error(1051, error.what());
            }
        });

    dispatcher.Register(
        {
            .name = "primitive.create",
            .description =
                "Creates a visible Primitive (box, sphere, cylinder, capsule "
                "or plane) in one undo step. `shape` (default sphere) and "
                "`surface` (standard | mirror | glass | emissive, default "
                "standard) pick the preset: a mirror is a perfect reflector, "
                "glass refracts the scene, reflects with Fresnel and focuses "
                "sunlight into caustics on nearby surfaces, an emissive "
                "radiates `color` at `emission_nits` (default 100000) and "
                "lights its surroundings. `position` (metres, relative to "
                "the parent's frame) is required; `parent` defaults to the "
                "primary viewport's target body. Optional: `name`, "
                "`euler_degrees`, `size` (full dimensions as [x,y,z] or one "
                "number), `color` (albedo, glass transmittance per metre or "
                "mirror tint), `roughness`, `metallic`, `ior` (glass, "
                "1..3), `caustics` (glass, default true). Returns id, name, "
                "shape and surface.",
            .mutating = true
        },
        [&session](const rpc::Value& params)
        {
            const auto& values = RequireObject(params);

            const auto shape = ParseShape(
                values.contains("shape") ? OptionalString(values, "shape")
                                         : std::string("sphere"));
            const auto surface =
                ParseSurface(OptionalString(values, "surface"));

            const auto position = OptionalVector(values, "position");
            if (!position.has_value())
            {
                throw rpc::Error(
                    -32602,
                    "position is required (metres, parent frame).");
            }

            auto& world = session.World();
            scene::ObjectId parent{};
            if (const std::string parentText = OptionalString(values, "parent");
                !parentText.empty())
            {
                const auto parsed = scene::ObjectId::Parse(parentText);
                if (!parsed.has_value() || !world.Objects().Find(*parsed))
                {
                    throw rpc::Error(1060, "parent is not an existing object.");
                }
                parent = *parsed;
            }
            else
            {
                const auto* target = session.Viewports().Find("studio.primary");
                const auto body =
                    target != nullptr && target->target.has_value()
                        ? world.Universe().ObjectForBody(target->target->body)
                        : std::nullopt;
                if (!body.has_value())
                {
                    throw rpc::Error(
                        1060,
                        "parent is required: the primary viewport has no "
                        "target body.");
                }
                parent = *body;
            }

            auto request = world_model::MakePrimitivePreset(shape, surface);
            if (const auto name = OptionalString(values, "name"); !name.empty())
            {
                request.name = name;
            }
            request.positionMeters = *position;
            if (const auto v = OptionalVector(values, "euler_degrees"))
            {
                request.eulerDegrees = *v;
            }
            if (const auto v = OptionalVector(values, "size", true))
            {
                request.sizeMeters = *v;
            }
            if (const auto v = OptionalVector(values, "color"))
            {
                request.color = *v;
            }
            if (const auto v = OptionalNumber(values, "roughness"))
            {
                request.roughness = *v;
            }
            if (const auto v = OptionalNumber(values, "metallic"))
            {
                request.metallic = *v;
            }
            if (const auto v = OptionalNumber(values, "ior"))
            {
                request.indexOfRefraction = *v;
            }
            if (const auto v = OptionalNumber(values, "emission_nits"))
            {
                request.emissionNits = *v;
            }
            if (const auto found = values.find("caustics");
                found != values.end() && found->second.IsBool())
            {
                request.caustics = found->second.AsBool();
            }

            try
            {
                const auto created = world_model::CreatePrimitive(
                    world.Commands(), parent, request);
                return rpc::Value(rpc::Value::Object{
                    {"id", created.ToString()},
                    {"name", request.name},
                    {"shape", static_cast<f64>(static_cast<int>(shape))},
                    {"surface", static_cast<f64>(static_cast<int>(surface))}});
            }
            catch (const rpc::Error&)
            {
                throw;
            }
            catch (const std::exception& error)
            {
                throw rpc::Error(1061, error.what());
            }
        });
}
} // namespace orbit::studio_session

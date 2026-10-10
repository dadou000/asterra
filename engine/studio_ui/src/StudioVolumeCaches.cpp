#include <orbit/studio_ui/StudioVolumeCaches.hpp>

#include <orbit/volume_render/UniversalVolumeRenderer.hpp>
#include <orbit/volume_representation/VolumeCache.hpp>
#include <orbit/weather_lab/CloudLayers.hpp>
#include <orbit/world_model/VolumeSchemas.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <utility>
#include <variant>

namespace orbit::studio_ui
{
namespace
{
using rpc::Value;

constexpr i64 kInvalid = 1090;
constexpr i64 kFailed = 1091;

// (volume, path) pairs that already failed to load, so a missing file is not
// retried and re-reported on every sync.
std::set<std::pair<scene::ObjectId, std::string>>& FailedLoads()
{
    static std::set<std::pair<scene::ObjectId, std::string>> failed;
    return failed;
}

[[nodiscard]] std::string CachePathOf(
    const scene::ObjectStore& objects, const scene::ObjectId volume)
{
    const auto value =
        objects.GetProperty(volume, world_model::kVolumeCachePath);
    if (value.has_value())
    {
        if (const auto* text = std::get_if<std::string>(&*value))
        {
            return *text;
        }
    }
    return {};
}

void Collect(
    const scene::ObjectStore& objects,
    const scene::ObjectRecord& record,
    std::vector<scene::ObjectId>& volumes)
{
    if (record.type == world_model::kVolumeType)
    {
        volumes.push_back(record.id);
    }
    for (const auto& child : objects.Children(record.id))
    {
        Collect(objects, child, volumes);
    }
}

[[nodiscard]] std::filesystem::path Resolve(
    const std::filesystem::path& root, const std::string& relative)
{
    const std::filesystem::path path(relative);
    return path.is_absolute() ? path : root / path;
}

[[nodiscard]] std::optional<scene::ObjectId> RequireVolume(
    const scene::ObjectStore& objects, const Value::Object& params)
{
    const auto found = params.find("id");
    if (found == params.end() || !found->second.IsString())
    {
        throw rpc::Error(-32602, "id must be a Volume object id.");
    }
    const auto id = scene::ObjectId::Parse(found->second.AsString());
    const auto record = id.has_value() ? objects.Find(*id) : std::nullopt;
    if (!record.has_value() || record->type != world_model::kVolumeType)
    {
        throw rpc::Error(kInvalid, "Object " + found->second.AsString() + " is not a Volume.");
    }
    return id;
}
} // namespace

u32 SyncVolumeCaches(
    const scene::ObjectStore& objects,
    const std::filesystem::path& projectRoot,
    std::string* log)
{
    std::vector<scene::ObjectId> volumes;
    for (const auto& root : objects.Roots())
    {
        Collect(objects, root, volumes);
    }

    u32 attached = 0U;
    for (const auto volume : volumes)
    {
        const std::string relative = CachePathOf(objects, volume);
        if (relative.empty() ||
            volume_representation::VolumeCaches().Has(volume) ||
            FailedLoads().contains({volume, relative}))
        {
            continue;
        }

        auto loaded = volume_representation::LoadVolumeCache(
            Resolve(projectRoot, relative).string());
        if (loaded)
        {
            volume_representation::VolumeCaches().Attach(
                volume, std::move(*loaded.cache));
            ++attached;
        }
        else
        {
            FailedLoads().insert({volume, relative});
            if (log != nullptr)
            {
                *log += "Volume cache '" + relative + "' not attached: " +
                    std::string(volume_representation::VolumeCacheLoadStatusName(
                        loaded.status)) +
                    " " + loaded.message + "\n";
            }
        }
    }
    return attached;
}

void RegisterVolumeCacheRpc(
    rpc::Dispatcher& dispatcher,
    studio_session::StudioSession& session)
{
    dispatcher.Register(
        {
            .name = "volume.cache_save",
            .description =
                "Saves the cache currently attached to a Volume (for example a "
                "Weather Lab storm pushed with weather_lab.volume) as a "
                ".orbitvol under the project and stores its project-relative "
                "path in the Volume's Baked Cache Path, so the Studio "
                "re-attaches it whenever the project opens. id = Volume "
                "object id; path optional (default Content/Volumes/<id>.orbitvol). "
                "One undoable property edit.",
            .mutating = true
        },
        [&session](const Value& params)
        {
            if (!session.World().HasWorld() || !params.IsObject())
            {
                throw rpc::Error(kFailed, "No world is open or params are not an object.");
            }
            const auto& object = params.AsObject();
            const auto& objects = session.World().Objects();
            const auto id = RequireVolume(objects, object);
            const auto* cache = volume_representation::VolumeCaches().Find(*id);
            if (cache == nullptr)
            {
                throw rpc::Error(kInvalid,
                    "The Volume has no attached cache; push a storm or import one first.");
            }

            std::string relative = "Content/Volumes/" + id->ToString() +
                std::string(volume_representation::kVolumeCacheExtension);
            if (const auto found = object.find("path");
                found != object.end() && found->second.IsString() &&
                !found->second.AsString().empty())
            {
                relative = found->second.AsString();
            }

            const auto root = session.World().Project().RootDirectory();
            const auto file = Resolve(root, relative);
            std::error_code error;
            std::filesystem::create_directories(file.parent_path(), error);
            std::string saveError;
            if (!volume_representation::SaveVolumeCache(file.string(), *cache, &saveError))
            {
                throw rpc::Error(kFailed, "Saving the cache failed: " + saveError);
            }

            auto& commands = session.World().Commands();
            commands.BeginTransaction("Set Volume Baked Cache Path");
            try
            {
                commands.SetProperty(*id, world_model::kVolumeCachePath, relative);
                commands.CommitTransaction();
            }
            catch (const std::exception& exception)
            {
                commands.RollbackTransaction();
                throw rpc::Error(kFailed, exception.what());
            }

            std::error_code sizeError;
            const auto bytes = std::filesystem::file_size(file, sizeError);
            return Value(Value::Object{
                {"path", relative},
                {"file", file.generic_string()},
                {"bytes", sizeError ? i64{0} : static_cast<i64>(bytes)}});
        });

    dispatcher.Register(
        {
            .name = "volume.cache_load",
            .description =
                "Attaches the .orbitvol named by a Volume's Baked Cache Path "
                "now (the Studio also does this when the project opens). "
                "Replaces an already attached cache. id = Volume object id.",
            .mutating = true
        },
        [&session](const Value& params)
        {
            if (!session.World().HasWorld() || !params.IsObject())
            {
                throw rpc::Error(kFailed, "No world is open or params are not an object.");
            }
            const auto& objects = session.World().Objects();
            const auto id = RequireVolume(objects, params.AsObject());
            const std::string relative = CachePathOf(objects, *id);
            if (relative.empty())
            {
                throw rpc::Error(kInvalid, "The Volume has no Baked Cache Path.");
            }
            auto loaded = volume_representation::LoadVolumeCache(
                Resolve(session.World().Project().RootDirectory(), relative).string());
            if (!loaded)
            {
                throw rpc::Error(kFailed,
                    std::string(volume_representation::VolumeCacheLoadStatusName(loaded.status)) +
                    ": " + loaded.message);
            }
            volume_representation::VolumeCaches().Attach(*id, std::move(*loaded.cache));
            FailedLoads().erase({*id, relative});
            return Value(Value::Object{{"attached", true}, {"path", relative}});
        });

    dispatcher.Register(
        {
            .name = "volume.look_variant",
            .description =
                "Gets or sets the look variant of generated cloud layers (0-7 forces one, 8 = auto: the genus' own) "
                "(Cloud Type 1-9): bit 0 domain warp + finer octave, bit 1 "
                "ridged noise, bit 2 high-contrast lighting. Optional: "
                "variant. Returns the active variant. A live shader A/B "
                "switch; 0 is the default look.",
            .mutating = true
        },
        [](const Value& params)
        {
            if (params.IsObject())
            {
                const auto found = params.AsObject().find("variant");
                if (found != params.AsObject().end() && found->second.IsNumber())
                {
                    volume_render::SetCloudLookVariant(
                        static_cast<u32>(std::max(0.0, found->second.AsNumber())));
                }
            }
            return Value(Value::Object{
                {"variant", static_cast<i64>(volume_render::CloudLookVariant())}});
        });

    dispatcher.Register(
        {
            .name = "volume.generate_layer",
            .description =
                "Fills a Volume with a procedural cloud layer and attaches it "
                "as its baked cache: kind = cirrus | cirrocumulus | cirrostratus | "
                "altocumulus | altostratus | nimbostratus | stratocumulus | "
                "stratus | cumulus (cumulonimbus is the simulated storm). "
                "Optional: seed, style (0-7: cumulus lobes / cirrus hooks), coverage (0-1), density (scales the baked "
                "density), wind [x, y]. With place=true the Volume is also "
                "configured as that layer in one undoable edit: Baked mode, "
                "Cloud Type, resolution 256, half extents (half_extent_xy, default "
                "the genus' recommended width, by the kind's default half thickness) and, when ground_position "
                "(a body-local point on the ground under the layer) is given, "
                "the centre at the kind's default height above it. With "
                "save=true the cache is also saved under the project (the "
                "Volume's Baked Cache Path) so it returns when the project "
                "opens.",
            .mutating = true
        },
        [&session](const Value& params)
        {
            if (!session.World().HasWorld() || !params.IsObject())
            {
                throw rpc::Error(kFailed, "No world is open or params are not an object.");
            }
            const auto& object = params.AsObject();
            const auto& objects = session.World().Objects();
            const auto id = RequireVolume(objects, object);

            const auto kindParam = object.find("kind");
            weather_lab::CloudLayerKind kind{};
            if (kindParam == object.end() || !kindParam->second.IsString() ||
                !weather_lab::ParseCloudLayerKind(kindParam->second.AsString(), kind) ||
                !weather_lab::IsGeneratedCloudLayer(kind))
            {
                throw rpc::Error(kInvalid,
                    "kind must be cirrus, cirrocumulus, cirrostratus, altocumulus, "
                    "altostratus, nimbostratus, stratocumulus, stratus or cumulus.");
            }

            const auto number = [&object](const char* key) -> std::optional<f64>
            {
                const auto found = object.find(key);
                if (found == object.end() || !found->second.IsNumber())
                {
                    return std::nullopt;
                }
                return found->second.AsNumber();
            };
            const auto boolean = [&object](const char* key)
            {
                const auto found = object.find(key);
                return found != object.end() && found->second.IsBool() && found->second.AsBool();
            };

            auto domain = world_model::ResolveVolumeDomain(objects, *id);
            if (!domain.has_value())
            {
                throw rpc::Error(kInvalid, "The Volume cannot be resolved.");
            }

            const bool place = boolean("place");
            const auto placement = weather_lab::DefaultCloudLayerPlacement(kind);
            math::Double3 halfExtents = domain->halfExtentsMeters;
            math::Double3 center = domain->centerMeters;
            u32 resolution = domain->resolution;
            if (place)
            {
                const f64 halfXY = number("half_extent_xy").value_or(placement.halfExtentMeters);
                halfExtents.x = halfXY;
                halfExtents.y = halfXY;
                halfExtents.z = placement.halfThicknessMeters;
                resolution = 256U;
                if (const auto ground = object.find("ground_position");
                    ground != object.end() && ground->second.IsArray() &&
                    ground->second.AsArray().size() == 3U)
                {
                    const auto& a = ground->second.AsArray();
                    const math::Double3 g{a[0].AsNumber(), a[1].AsNumber(), a[2].AsNumber()};
                    const f64 length = std::sqrt(g.x * g.x + g.y * g.y + g.z * g.z);
                    if (!(length > 0.0))
                    {
                        throw rpc::Error(kInvalid, "ground_position must not be the planet centre.");
                    }
                    const f64 lift = placement.centerAboveGroundMeters / length;
                    center = {g.x + g.x * lift, g.y + g.y * lift, g.z + g.z * lift};
                }
            }

            weather_lab::CloudLayerSettings settings;
            settings.kind = kind;
            settings.seed = static_cast<std::uint32_t>(number("seed").value_or(1.0));
            settings.resolutionX = resolution;
            settings.resolutionY = resolution;
            settings.resolutionZ = resolution;
            settings.halfExtentX = halfExtents.x;
            settings.halfExtentY = halfExtents.y;
            settings.halfExtentZ = halfExtents.z;
            settings.coverage = static_cast<float>(number("coverage").value_or(0.55));
            settings.density = static_cast<float>(number("density").value_or(1.0));
            settings.style = static_cast<std::uint32_t>(std::clamp(
                number("style").value_or(static_cast<f64>(cloud_genera::GenusInfo(kind).defaultStyle)), 0.0, 7.0));
            if (const auto wind = object.find("wind");
                wind != object.end() && wind->second.IsArray() && wind->second.AsArray().size() == 2U)
            {
                settings.windX = static_cast<float>(wind->second.AsArray()[0].AsNumber());
                settings.windY = static_cast<float>(wind->second.AsArray()[1].AsNumber());
            }

            volume_representation::VolumeCacheData cache;
            cache.descriptor.resolutionX = settings.resolutionX;
            cache.descriptor.resolutionY = settings.resolutionY;
            cache.descriptor.resolutionZ = settings.resolutionZ;
            cache.descriptor.fieldMask = static_cast<u64>(world_model::VolumeField::Density);
            cache.descriptor.centerMeters = center;
            cache.descriptor.halfExtentsMeters = halfExtents;
            cache.density = weather_lab::GenerateCloudLayerDensity(settings);
            volume_representation::FinalizeVolumeCache(cache);

            auto& commands = session.World().Commands();
            const bool save = boolean("save");
            std::string relative;
            if (save)
            {
                relative = "Content/Volumes/" + id->ToString() +
                    std::string(volume_representation::kVolumeCacheExtension);
                const auto file = Resolve(session.World().Project().RootDirectory(), relative);
                std::error_code error;
                std::filesystem::create_directories(file.parent_path(), error);
                std::string saveError;
                if (!volume_representation::SaveVolumeCache(file.string(), cache, &saveError))
                {
                    throw rpc::Error(kFailed, "Saving the cache failed: " + saveError);
                }
            }

            if (place || save)
            {
                commands.BeginTransaction("Generate Cloud Layer");
                try
                {
                    if (place)
                    {
                        commands.SetProperty(*id, world_model::kVolumeRepresentationMode, i64{4});
                        commands.SetProperty(*id, world_model::kVolumeCloudType,
                            static_cast<i64>(weather_lab::CloudTypeCode(kind)));
                        commands.SetProperty(*id, world_model::kVolumeResolution, static_cast<i64>(resolution));
                        commands.SetProperty(*id, world_model::kVolumeHalfExtentsMeters, halfExtents);
                        commands.SetProperty(*id, world_model::kVolumeCenterMeters, center);
                        commands.SetProperty(*id, world_model::kVolumeExtinctionScale, placement.extinctionScale);
                    }
                    if (save)
                    {
                        commands.SetProperty(*id, world_model::kVolumeCachePath, relative);
                    }
                    commands.CommitTransaction();
                }
                catch (const std::exception& exception)
                {
                    commands.RollbackTransaction();
                    throw rpc::Error(kFailed, exception.what());
                }
            }

            volume_representation::VolumeCaches().Attach(*id, std::move(cache));
            return Value(Value::Object{
                {"kind", std::string(weather_lab::CloudLayerKindName(kind))},
                {"cloud_type", static_cast<i64>(weather_lab::CloudTypeCode(kind))},
                {"resolution", static_cast<i64>(resolution)},
                {"saved_path", relative},
                {"center", Value(Value::Array{center.x, center.y, center.z})},
                {"half_extents", Value(Value::Array{halfExtents.x, halfExtents.y, halfExtents.z})}});
        });
}
} // namespace orbit::studio_ui

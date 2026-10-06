#include <orbit/studio_session/StudioTerrainStatusRpc.hpp>

#include <orbit/scene/ObjectStore.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/studio_session/StudioTerrainServiceStatus.hpp>

#include <string>
#include <string_view>

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

[[nodiscard]] rpc::Value CacheStatsToRpc(
    const terrain_gpu::PersistentGpuTerrainCacheStats& stats)
{
    const f64 lookups =
        static_cast<f64>(stats.hits + stats.misses);

    return rpc::Value(
        rpc::Value::Object{
            {"hits", static_cast<i64>(stats.hits)},
            {"misses", static_cast<i64>(stats.misses)},
            {"generations", static_cast<i64>(stats.generations)},
            {"insertions", static_cast<i64>(stats.insertions)},
            {"evictions", static_cast<i64>(stats.evictions)},
            {"resident_pages", static_cast<i64>(stats.residentPages)},
            {"resident_bytes", static_cast<i64>(stats.residentBytes)},
            {"hit_rate_percent",
             lookups > 0.0
                 ? 100.0 * static_cast<f64>(stats.hits) / lookups
                 : 0.0}
        });
}
} // namespace

void RegisterStudioTerrainStatusRpc(
    rpc::Dispatcher& dispatcher,
    StudioSession& session)
{
    dispatcher.Register(
        {
            .name = "terrain.cache_stats",
            .description =
                "Persistent GPU terrain cache statistics (hits, misses, generations, evictions, resident pages and bytes) for a terrain object, plus the stationary-camera cache counters of a viewport. After warmup with a stationary camera hits may grow but misses and generations must not.",
            .mutating = false
        },
        [&session](const rpc::Value& params)
        {
            const auto& values = RequireObject(params);

            const auto terrain =
                scene::ObjectId::Parse(
                    RequireString(values, "terrain"));

            if (!terrain.has_value())
            {
                throw rpc::Error(
                    -32602,
                    "terrain must be a terrain object id.");
            }

            std::string viewport = "studio.primary";

            if (const auto found = values.find("viewport");
                found != values.end())
            {
                if (!found->second.IsString() ||
                    found->second.AsString().empty())
                {
                    throw rpc::Error(
                        -32602,
                        "viewport must be a non-empty string.");
                }

                viewport = found->second.AsString();
            }

            const auto status =
                StudioTerrainStatusInspector::Capture(
                    session,
                    *terrain,
                    nullptr,
                    viewport);

            if (!status.has_value())
            {
                throw rpc::Error(
                    1004,
                    "No terrain services for that object (is a world open and the object a terrain surface?).");
            }

            const auto performance =
                session.TerrainPerformance().Capture(
                    session,
                    viewport);

            rpc::Value::Object stationary{
                {"frames",
                 static_cast<i64>(performance.stationaryFrames)},
                {"cache_hits",
                 static_cast<i64>(performance.stationaryCacheHits)},
                {"cache_misses",
                 static_cast<i64>(performance.stationaryCacheMisses)},
                {"hit_rate_percent",
                 performance.stationaryCacheHitRatePercent}
            };

            return rpc::Value(
                rpc::Value::Object{
                    {"terrain", terrain->ToString()},
                    {"viewport", status->selectedViewport},
                    {"cache", CacheStatsToRpc(status->cacheStats)},
                    {"stationary",
                     rpc::Value(std::move(stationary))},
                    {"physical_lod",
                     status->selectedPhysicalLod.has_value()
                         ? rpc::Value(
                               static_cast<i64>(
                                   *status->selectedPhysicalLod))
                         : rpc::Value{}},
                    {"semantic_revision",
                     static_cast<i64>(status->semanticRevision)},
                    {"surface_source_revision",
                     static_cast<i64>(status->surfaceSourceRevision)}
                });
        });
}
} // namespace orbit::studio_session

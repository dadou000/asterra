#include <orbit/studio_ui/StudioFlatMapRpc.hpp>

#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>

namespace orbit::studio_ui
{
namespace
{
using rpc::Value;

constexpr i64 kInvalid = 1090;
constexpr i64 kFailed = 1091;

[[nodiscard]] std::string ViewIdOrPrimary(const Value& params)
{
    if (params.IsObject())
    {
        const auto found = params.AsObject().find("id");
        if (found != params.AsObject().end() && found->second.IsString() &&
            !found->second.AsString().empty())
        {
            return found->second.AsString();
        }
    }
    return "studio.primary";
}

[[nodiscard]] std::optional<f64> OptionalNumber(
    const Value& params,
    const char* key)
{
    if (!params.IsObject())
    {
        return std::nullopt;
    }
    const auto found = params.AsObject().find(key);
    if (found == params.AsObject().end() || found->second.IsNull())
    {
        return std::nullopt;
    }
    if (!found->second.IsNumber())
    {
        throw rpc::Error(
            -32602, std::string(key) + " must be a number.");
    }
    return found->second.AsNumber();
}

[[nodiscard]] Value LatLonToRpc(const FlatMapLatLon& latLon)
{
    return Value(Value::Object{
        {"latitude_degrees", latLon.latitudeDegrees},
        {"longitude_degrees", latLon.longitudeDegrees}});
}

[[nodiscard]] Value StatusToRpc(
    const StudioRenderViewSet& views,
    const std::string& id)
{
    Value::Array layers;
    for (u32 index = 0U; index < kFlatMapLayerCount; ++index)
    {
        layers.emplace_back(
            std::string(FlatMapLayerName(static_cast<FlatMapLayer>(index))));
    }

    Value::Object result{
        {"id", id},
        {"layer", std::string(FlatMapLayerName(views.FlatMapLayerOf(id)))},
        {"layers", Value(std::move(layers))}};

    if (const auto status = views.FlatMapStatusOf(id); status.has_value())
    {
        result.emplace("has_source", status->hasSource);
        result.emplace("rows_generated", static_cast<i64>(status->rowsGenerated));
        result.emplace("rows_total", static_cast<i64>(status->rowsTotal));
        result.emplace("complete", status->complete);
        result.emplace(
            "marker",
            status->markerValid ? LatLonToRpc(status->marker) : Value());
    }
    else
    {
        // The view has not drawn a map yet (it is not in flat_map mode).
        result.emplace("has_source", false);
        result.emplace("rows_generated", static_cast<i64>(0));
        result.emplace("rows_total", static_cast<i64>(kFlatMapHeight));
        result.emplace("complete", false);
        result.emplace("marker", Value());
    }
    return Value(std::move(result));
}
} // namespace

void RegisterStudioFlatMapRpc(
    rpc::Dispatcher& dispatcher,
    StudioRenderViewSet& views)
{
    dispatcher.Register(
        {
            .name = "map.open",
            .description =
                "Opens the flat planet map in a viewport: switches it to the "
                "flat_map viewport mode (the same as choosing Flat Map in the "
                "viewport mode selector). The map is an equirectangular image "
                "of the viewport's target planet with a lat/long grid and a "
                "marker for the camera; it is generated progressively, so poll "
                "map.status until complete. Switch back with view.mode_set "
                "perspective. id defaults to studio.primary.",
            .mutating = true
        },
        [&views](const Value& params)
        {
            const std::string id = ViewIdOrPrimary(params);
            try
            {
                views.SetViewportMode(
                    id, studio_session::ViewportMode::FlatMap);
            }
            catch (const std::exception& exception)
            {
                throw rpc::Error(kInvalid, exception.what());
            }
            return StatusToRpc(views, id);
        });

    dispatcher.Register(
        {
            .name = "map.status",
            .description =
                "Returns the flat map state of a viewport: the active layer "
                "and the available layers, whether a terrain source is "
                "bound, how many raster rows are generated (rows_generated / "
                "rows_total, complete) and the camera marker as latitude and "
                "longitude in degrees (null when unknown). Latitude and "
                "longitude use the same convention as the viewport text HUD. "
                "id defaults to studio.primary.",
            .mutating = false
        },
        [&views](const Value& params)
        {
            return StatusToRpc(views, ViewIdOrPrimary(params));
        });

    dispatcher.Register(
        {
            .name = "map.layer_set",
            .description =
                "Chooses what the flat map colours the planet by: "
                "elevation, biomes, temperature, precipitation or "
                "water_depth. Switching layers does not re-sample the planet. "
                "id defaults to studio.primary.",
            .mutating = true
        },
        [&views](const Value& params)
        {
            if (!params.IsObject())
            {
                throw rpc::Error(-32602, "Params must be an object.");
            }
            const auto found = params.AsObject().find("layer");
            if (found == params.AsObject().end() || !found->second.IsString())
            {
                throw rpc::Error(-32602, "layer must be a string.");
            }
            const auto layer = ParseFlatMapLayer(found->second.AsString());
            if (!layer.has_value())
            {
                throw rpc::Error(
                    -32602,
                    "layer must be elevation, biomes, temperature, "
                    "precipitation or water_depth.");
            }

            const std::string id = ViewIdOrPrimary(params);
            try
            {
                views.SetFlatMapLayer(id, *layer);
            }
            catch (const std::exception& exception)
            {
                throw rpc::Error(kInvalid, exception.what());
            }
            return StatusToRpc(views, id);
        });

    dispatcher.Register(
        {
            .name = "map.travel",
            .description =
                "Travels to a point on the planet shown by the flat map: the "
                "terrain camera of the viewport moves to a low vantage over "
                "that spot, exactly like double-clicking the map. Give "
                "latitude_degrees and longitude_degrees, or u and v (0..1, "
                "origin top-left) for a position inside the viewport that "
                "currently shows the flat map or the globe (body_map mode). By default the viewport then "
                "returns to perspective mode (perspective=false stays on the "
                "map, where the marker follows). Fails when u/v fall on the "
                "letterbox bars or the viewport has no current terrain "
                "runtime. id defaults to studio.primary.",
            .mutating = true
        },
        [&views](const Value& params)
        {
            const std::string id = ViewIdOrPrimary(params);

            std::optional<FlatMapLatLon> target;
            const auto latitude = OptionalNumber(params, "latitude_degrees");
            const auto longitude = OptionalNumber(params, "longitude_degrees");
            if (latitude.has_value() || longitude.has_value())
            {
                if (!latitude.has_value() || !longitude.has_value())
                {
                    throw rpc::Error(
                        -32602,
                        "latitude_degrees and longitude_degrees must be "
                        "given together.");
                }
                if (std::abs(*latitude) > 90.0)
                {
                    throw rpc::Error(
                        -32602, "latitude_degrees must be within -90..90.");
                }
                target = FlatMapLatLon{*latitude, *longitude};
            }
            else
            {
                const auto u = OptionalNumber(params, "u");
                const auto v = OptionalNumber(params, "v");
                if (!u.has_value() || !v.has_value())
                {
                    throw rpc::Error(
                        -32602,
                        "Give latitude_degrees and longitude_degrees, or u "
                        "and v.");
                }
                const auto direction = views.PickPlanetDirection(
                    id, static_cast<f32>(*u), static_cast<f32>(*v));
                if (!direction.has_value())
                {
                    throw rpc::Error(
                        kInvalid,
                        "u and v do not hit the planet: the viewport must "
                        "be in flat_map or body_map mode and the position "
                        "must fall on the map (not the letterbox bars) or "
                        "on the globe.");
                }
                target = FlatMapLatLonFromDirection(*direction);
            }

            bool returnToPerspective = true;
            if (params.IsObject())
            {
                const auto found = params.AsObject().find("perspective");
                if (found != params.AsObject().end() && found->second.IsBool())
                {
                    returnToPerspective = found->second.AsBool();
                }
            }

            try
            {
                if (!views.FocusTerrainDirection(
                        id,
                        FlatMapDirectionFromLatLon(
                            target->latitudeDegrees,
                            target->longitudeDegrees)))
                {
                    throw rpc::Error(
                        kFailed,
                        "The viewport has no current terrain runtime to "
                        "travel with.");
                }
                if (returnToPerspective)
                {
                    views.SetViewportMode(
                        id, studio_session::ViewportMode::Perspective);
                }
            }
            catch (const rpc::Error&)
            {
                throw;
            }
            catch (const std::exception& exception)
            {
                throw rpc::Error(kFailed, exception.what());
            }

            Value::Object result{
                {"id", id},
                {"traveled", true},
                {"target", LatLonToRpc(*target)},
                {"perspective", returnToPerspective}};
            return Value(std::move(result));
        });
}
} // namespace orbit::studio_ui

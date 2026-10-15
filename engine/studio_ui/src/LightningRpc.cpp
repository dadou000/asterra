#include <orbit/studio_ui/LightningRpc.hpp>

#include <cmath>
#include <optional>
#include <string>

namespace orbit::studio_ui
{
namespace
{
using rpc::Value;
constexpr i64 kBadParams = -32602;

[[nodiscard]] const Value::Object& Params(const Value& params)
{
    static const Value::Object empty;
    return params.IsObject() ? params.AsObject() : empty;
}

[[nodiscard]] std::optional<f64> Number(const Value::Object& o, const char* key, const f64 lo, const f64 hi)
{
    const auto found = o.find(key);
    if (found == o.end() || found->second.IsNull())
    {
        return std::nullopt;
    }
    const f64 v = found->second.IsNumber() ? found->second.AsNumber() : std::nan("");
    if (!std::isfinite(v) || v < lo || v > hi)
    {
        throw rpc::Error(kBadParams, std::string(key) + " must be a number in [" + std::to_string(lo) + ", " + std::to_string(hi) + "].");
    }
    return v;
}

[[nodiscard]] Value StatusValue(const std::vector<lighting::FlashStatus>& flashes)
{
    Value::Array list;
    for (const auto& f : flashes)
    {
        list.push_back(Value::Object{
            {"id", static_cast<f64>(f.id)},
            {"placed", f.placed},
            {"age_s", f.ageSeconds},
            {"remaining_s", f.remainingSeconds},
            {"base_m", Value::Array{f.baseMeters.x, f.baseMeters.y, f.baseMeters.z}},
        });
    }
    return Value::Object{{"count", static_cast<f64>(flashes.size())}, {"flashes", std::move(list)}};
}
} // namespace

void RegisterLightningRpc(rpc::Dispatcher& dispatcher, LightningHost host)
{
    dispatcher.Register(
        {
            .name = "lightning.fire",
            .description =
                "Fires one lightning flash that lights the lit scene and (with ORBIT_CLOUD_POINT_LIGHTS=1) the clouds. "
                "By default the channel stands `distance_m` (default 3000) ahead of the camera, `lateral_m` to its right, "
                "on the horizontal plane, rising `height_m` (default 6000) along the camera up; the position is fixed at "
                "the first frame. Pass `base_m` ([x, y, z] in the lighting frame) to place it absolutely. `energy_j` "
                "(optical, default 3e5), `strokes` (1-8, default 3), `stroke_spacing_s` (default 0.06) and `seed` shape "
                "the flash. The flash lives about 0.25 s after its last stroke. Returns the flash id.",
            .mutating = true
        },
        [host](const Value& params)
        {
            const auto& o = Params(params);
            lighting::FlashRequest request;
            if (const auto v = Number(o, "distance_m", 1.0, 1.0e6)) { request.distanceMeters = *v; }
            if (const auto v = Number(o, "lateral_m", -1.0e6, 1.0e6)) { request.lateralMeters = *v; }
            if (const auto v = Number(o, "height_m", 10.0, 2.0e4)) { request.heightMeters = *v; }
            if (const auto v = Number(o, "energy_j", 0.0, 1.0e8)) { request.energyJoules = *v; }
            if (const auto v = Number(o, "strokes", 1.0, 8.0)) { request.strokes = static_cast<u32>(*v); }
            if (const auto v = Number(o, "stroke_spacing_s", 0.0, 1.0)) { request.strokeSpacingSeconds = *v; }
            if (const auto v = Number(o, "seed", 0.0, 4.0e9)) { request.seed = static_cast<u32>(*v); }
            if (const auto found = o.find("base_m"); found != o.end() && !found->second.IsNull())
            {
                const bool valid = found->second.IsArray() && found->second.AsArray().size() == 3;
                if (!valid)
                {
                    throw rpc::Error(kBadParams, "base_m must be [x, y, z].");
                }
                f64 xyz[3];
                for (int i = 0; i < 3; ++i)
                {
                    const Value& c = found->second.AsArray()[static_cast<std::size_t>(i)];
                    if (!c.IsNumber() || !std::isfinite(c.AsNumber()))
                    {
                        throw rpc::Error(kBadParams, "base_m must contain finite numbers.");
                    }
                    xyz[i] = c.AsNumber();
                }
                request.cameraRelative = false;
                request.baseMeters = {xyz[0], xyz[1], xyz[2]};
            }
            const u64 id = host.fire(request);
            return Value::Object{{"id", static_cast<f64>(id)}};
        });

    dispatcher.Register(
        {
            .name = "lightning.status",
            .description = "Live lightning flashes: id, whether the position has been resolved by a frame, age, time left and base position.",
            .mutating = false
        },
        [host](const Value&) { return StatusValue(host.status()); });

    dispatcher.Register(
        {
            .name = "lightning.storm_start",
            .description =
                "Turns the Weather Lab's live storm into lightning. Needs a live Weather Lab sample (weather_lab.control start "
                "or step) with square cells (dx = dz, for example nx=ny=64, nz=40, dx=dz=500). The ice phase is derived from "
                "temperature (an assumption), the electrification runs on a worker thread at `time_scale` simulated seconds "
                "per wall second (default 20) on a frozen snapshot, and each flash is placed against a fixed spot "
                "`distance_m` (default 12000) ahead of the camera and `lateral_m` to its right, fixed at the first frame. "
                "A young storm takes minutes of simulated time to flash. Restarting takes a fresh snapshot.",
            .mutating = true
        },
        [host](const Value& params)
        {
            const auto& o = Params(params);
            StormLightningRequest request;
            if (const auto v = Number(o, "distance_m", 1.0, 1.0e6)) { request.distanceMeters = *v; }
            if (const auto v = Number(o, "lateral_m", -1.0e6, 1.0e6)) { request.lateralMeters = *v; }
            if (const auto v = Number(o, "time_scale", 1.0, 1000.0)) { request.timeScale = *v; }
            const std::string error = host.stormStart ? host.stormStart(request) : "storm lightning is not available";
            if (!error.empty())
            {
                throw rpc::Error(1100, error);
            }
            return Value::Object{{"started", true}};
        });

    dispatcher.Register(
        {
            .name = "lightning.storm_stop",
            .description = "Stops the Weather Lab driven lightning (flashes already alive finish).",
            .mutating = true
        },
        [host](const Value&)
        {
            if (host.stormStop) { host.stormStop(); }
            return Value::Object{{"stopped", true}};
        });

    dispatcher.Register(
        {
            .name = "lightning.storm_status",
            .description = "Weather Lab driven lightning: running, simulated seconds, flashes so far, the strongest field "
                           "as a multiple of the breakdown field (1 or more = flashing), and the storm domain size in metres.",
            .mutating = false
        },
        [host](const Value&)
        {
            const StormLightningStatus s = host.stormStatus ? host.stormStatus() : StormLightningStatus{};
            return Value::Object{
                {"running", s.running}, {"simulated_s", s.simulatedSeconds}, {"flashes", static_cast<f64>(s.flashes)},
                {"breakdown_ratio", s.breakdownRatio}, {"domain_m", s.domainMeters}};
        });

    dispatcher.Register(
        {
            .name = "lightning.clear",
            .description = "Removes every live lightning flash.",
            .mutating = true
        },
        [host](const Value&)
        {
            host.clear();
            return Value::Object{{"cleared", true}};
        });
}
} // namespace orbit::studio_ui

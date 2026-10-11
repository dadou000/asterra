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

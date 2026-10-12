#include <orbit/studio_ui/StormDomain.hpp>

#include <cmath>
#include <cstdio>

namespace orbit::studio_ui
{
namespace
{
using rpc::Value;
constexpr i64 kBadParams = -32602;

bool IsPowerOfTwo(const u32 v) { return v >= 2U && (v & (v - 1U)) == 0U; }

Value StatusValue(const StormStatus& s)
{
    return Value::Object{
        {"state", std::string(StormStateName(s.state))},
        {"mode", s.mode},
        {"message", s.message},
        {"latitude_deg", s.latitudeDegrees},
        {"longitude_deg", s.longitudeDegrees},
        {"storm_seconds", s.stormSeconds},
        {"steps", static_cast<f64>(s.steps)},
        {"step_ms", s.stepMilliseconds},
        {"cells", static_cast<f64>(s.cells)},
        {"spacing_m", static_cast<f64>(s.spacingMeters)},
        {"resident_mib", s.residentMiB},
        {"storm_id", [&] { char text[17]; std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(s.stormId)); return std::string(text); }()},
        {"age_seconds", s.ageSeconds},
        {"lifetime_seconds", s.lifetimeSeconds},
        {"intensity", s.intensity},
        {"catching_up", s.catchingUp},
        {"checkpoints", static_cast<f64>(s.checkpoints)},
        {"checkpoint_mib", s.checkpointMiB},
        {"lightning_flashes", static_cast<f64>(s.lightningFlashes)},
        {"scheduled_nearby", static_cast<f64>(s.scheduledNearby)},
        {"shader_reload", s.shaderReload},
        {"shader_reloads", static_cast<f64>(s.shaderReloads)},
        {"nested", s.nested},
        {"child_spacing_m", static_cast<f64>(s.childSpacingMeters)}};
}

f64 Optional(const Value& params, const char* key, const f64 fallback)
{
    const Value* v = params.IsObject() ? params.Find(key) : nullptr;
    if (v == nullptr) { return fallback; }
    if (!v->IsNumber()) { throw rpc::Error(kBadParams, std::string(key) + " must be a number."); }
    return v->AsNumber();
}
} // namespace

const char* StormStateName(const StormStatus::State state) noexcept
{
    switch (state)
    {
    case StormStatus::State::None: return "none";
    case StormStatus::State::Pending: return "pending";
    case StormStatus::State::Active: return "active";
    case StormStatus::State::Failed: return "failed";
    }
    return "none";
}

std::string StormDomainMailbox::Place(const StormPlaceRequest& request)
{
    if (!(std::fabs(request.latitudeDegrees) <= 90.0) || !(std::fabs(request.longitudeDegrees) <= 360.0))
    {
        return "latitude must be within +-90 degrees and longitude within +-360";
    }
    if (!IsPowerOfTwo(request.cells) || request.cells < 16U || request.cells > 256U)
    {
        return "cells must be a power of two from 16 to 256";
    }
    if (!(request.spacingMeters >= 500.0F && request.spacingMeters <= 5000.0F))
    {
        return "spacing must be 500 to 5000 m";
    }
    if (!(request.spinMinutes >= 0.0F && request.spinMinutes <= 240.0F) || !(request.bubbleKelvin >= 0.0F && request.bubbleKelvin <= 6.0F))
    {
        return "spin_minutes must be 0 to 240 and bubble_k 0 to 6";
    }
    if (request.spongeCells > request.cells / 4U)
    {
        return "sponge_cells must be at most a quarter of the cells";
    }
    const std::scoped_lock lock(mutex_);
    place_ = request;
    clear_ = false;
    status_ = {};
    status_.state = StormStatus::State::Pending;
    status_.message = "waiting for a frame that draws clouds on the body under the primary view";
    status_.latitudeDegrees = request.latitudeDegrees;
    status_.longitudeDegrees = request.longitudeDegrees;
    status_.cells = request.cells;
    status_.spacingMeters = request.spacingMeters;
    return {};
}

std::string StormDomainMailbox::SetAuto(const StormAutoRequest& request)
{
    if (!IsPowerOfTwo(request.cells) || request.cells < 16U || request.cells > 256U)
    {
        return "cells must be a power of two from 16 to 256";
    }
    if (!(request.spacingMeters >= 500.0F && request.spacingMeters <= 5000.0F))
    {
        return "spacing must be 500 to 5000 m";
    }
    if (!(request.searchKilometres >= 50.0F && request.searchKilometres <= 2000.0F))
    {
        return "search must be 50 to 2000 km";
    }
    const std::scoped_lock lock(mutex_);
    auto_ = request;
    autoSetting_ = request;
    return {};
}

StormAutoRequest StormDomainMailbox::AutoSetting() const
{
    const std::scoped_lock lock(mutex_);
    return autoSetting_;
}

std::optional<StormAutoRequest> StormDomainMailbox::TakeAuto()
{
    const std::scoped_lock lock(mutex_);
    auto request = auto_;
    auto_.reset();
    return request;
}

std::string StormDomainMailbox::SetLightning(const StormLightningSettings& settings)
{
    if (!(settings.slowMotion >= 1.0 && settings.slowMotion <= 2000.0)) { return "slow_motion must be 1 to 2000"; }
    if (!(settings.chargeBoost >= 1.0 && settings.chargeBoost <= 10000.0)) { return "charge_boost must be 1 to 10000"; }
    if (!(settings.breakdownScale >= 0.02 && settings.breakdownScale <= 1.0)) { return "breakdown_scale must be 0.02 to 1"; }
    const std::scoped_lock lock(mutex_);
    lightning_ = settings;
    return {};
}

StormLightningSettings StormDomainMailbox::Lightning() const
{
    const std::scoped_lock lock(mutex_);
    return lightning_;
}

void StormDomainMailbox::RequestShaderReload(const std::string& path)
{
    const std::scoped_lock lock(mutex_);
    reload_ = path;
}

std::optional<std::string> StormDomainMailbox::TakeShaderReload()
{
    const std::scoped_lock lock(mutex_);
    auto path = reload_;
    reload_.reset();
    return path;
}

void StormDomainMailbox::PublishShaderReload(const std::string& message, const bool succeeded)
{
    const std::scoped_lock lock(mutex_);
    reloadMessage_ = message;
    if (succeeded) { ++reloadCount_; }
    status_.shaderReload = reloadMessage_;
    status_.shaderReloads = reloadCount_;
}

void StormDomainMailbox::Clear()
{
    const std::scoped_lock lock(mutex_);
    place_.reset();
    clear_ = true;
    status_ = {};
}

StormStatus StormDomainMailbox::Status() const
{
    const std::scoped_lock lock(mutex_);
    return status_;
}

std::optional<StormPlaceRequest> StormDomainMailbox::TakePlace()
{
    const std::scoped_lock lock(mutex_);
    auto request = place_;
    place_.reset();
    return request;
}

bool StormDomainMailbox::TakeClear()
{
    const std::scoped_lock lock(mutex_);
    const bool clear = clear_;
    clear_ = false;
    return clear;
}

void StormDomainMailbox::Publish(const StormStatus& status)
{
    const std::scoped_lock lock(mutex_);
    // A queued request or clear that arrived meanwhile wins over a stale status from the render thread.
    if (place_.has_value() || clear_) { return; }
    status_ = status;
    status_.shaderReload = reloadMessage_;
    status_.shaderReloads = reloadCount_;
}

void RegisterStormDomainRpc(rpc::Dispatcher& dispatcher, StormDomainMailbox& mailbox)
{
    dispatcher.Register(
        {
            .name = "storm.status",
            .description = "The simulated storm domain: `state` (none, pending, active, failed), `message`, its centre `latitude_deg` / "
                           "`longitude_deg` (it drifts with the storm), `storm_seconds` simulated, `steps`, `step_ms` (GPU time per step), "
                           "`cells`, `spacing_m` and `resident_mib`.",
            .mutating = false
        },
        [&mailbox](const Value&) { return StatusValue(mailbox.Status()); });

    dispatcher.Register(
        {
            .name = "storm.place",
            .description =
                "Places a simulated storm domain at a latitude / longitude on the body under the primary view. The GPU storm core "
                "(engine/weather_gpu) is built on the next frame that draws clouds, spun up for `spin_minutes` (default 40) and then "
                "advanced in step with the simulation clock; inside the domain the cloud renderer draws the simulated condensate "
                "instead of the procedural clouds, in the march, the cloud shadow and the sun light volume. Parameters: `latitude_deg`, "
                "`longitude_deg`, `cells` (nx = ny, power of two 16 to 256, default 128), `spacing_m` (500 to 5000, default 2000), "
                "`bubble_k` (default 2), `spin_minutes` (0 to 240), `ice` (default true), `sponge_cells` (default 12), `nested` (default true: a 4:1 child grid over the middle quarter of the domain, so 500 m inside a 2 km domain). One domain at a "
                "time: placing replaces it. Returns `error` (empty when queued) and the status; poll `storm.status` for `active`. "
                "Compare it with the procedural sky using clouds.split_set with `CLOUD_STORM=0` for the right half.",
            .mutating = true
        },
        [&mailbox](const Value& params)
        {
            if (!params.IsObject() || params.Find("latitude_deg") == nullptr || params.Find("longitude_deg") == nullptr)
            {
                throw rpc::Error(kBadParams, "latitude_deg and longitude_deg are required.");
            }
            StormPlaceRequest request;
            request.latitudeDegrees = Optional(params, "latitude_deg", 0.0);
            request.longitudeDegrees = Optional(params, "longitude_deg", 0.0);
            request.cells = static_cast<u32>(Optional(params, "cells", request.cells));
            request.spacingMeters = static_cast<f32>(Optional(params, "spacing_m", request.spacingMeters));
            request.bubbleKelvin = static_cast<f32>(Optional(params, "bubble_k", request.bubbleKelvin));
            request.spinMinutes = static_cast<f32>(Optional(params, "spin_minutes", request.spinMinutes));
            request.spongeCells = static_cast<u32>(Optional(params, "sponge_cells", request.spongeCells));
            if (const Value* ice = params.Find("ice"); ice != nullptr)
            {
                if (!ice->IsBool()) { throw rpc::Error(kBadParams, "ice must be a boolean."); }
                request.ice = ice->AsBool();
            }
            if (const Value* nested = params.Find("nested"); nested != nullptr)
            {
                if (!nested->IsBool()) { throw rpc::Error(kBadParams, "nested must be a boolean."); }
                request.nested = nested->AsBool();
            }
            const std::string error = mailbox.Place(request);
            Value::Object result{{"error", error}};
            for (auto& entry : StatusValue(mailbox.Status()).AsObject()) { result.insert(entry); }
            return result;
        });

    dispatcher.Register(
        {
            .name = "storm.auto",
            .description =
                "Scheduled storms near the camera. `enabled` true makes Studio simulate the nearest live storm of the deterministic storm "
                "schedule (engine/celestial_clouds StormSchedule) within `search_km` (default 500) of the camera: a pure function of the "
                "absolute simulation time and `seed` (default 1), so the same time shows the same storm in every launch, and a time jump "
                "recomputes the storm from the nearest cached checkpoint instead of stepping through the skipped time. The storm grows from "
                "nothing at its start, lives 2.5 to 4.5 hours and dissolves into the procedural clouds. Other parameters: `cells` (power of "
                "two 16 to 256, default 128), `spacing_m` (default 2000), `ice` (default true), `nested` (default true). A manual storm.place overrides it while it "
                "exists. Returns `error` and the status (`mode`, `storm_id`, `age_seconds`, `lifetime_seconds`, `intensity`, `catching_up`, "
                "`checkpoints`, `checkpoint_mib`, `scheduled_nearby`).",
            .mutating = true
        },
        [&mailbox](const Value& params)
        {
            const Value* enabled = params.IsObject() ? params.Find("enabled") : nullptr;
            if (enabled == nullptr || !enabled->IsBool())
            {
                throw rpc::Error(kBadParams, "enabled must be a boolean.");
            }
            StormAutoRequest request = mailbox.AutoSetting();
            request.enabled = enabled->AsBool();
            request.seed = static_cast<u64>(Optional(params, "seed", static_cast<f64>(request.seed)));
            request.cells = static_cast<u32>(Optional(params, "cells", request.cells));
            request.spacingMeters = static_cast<f32>(Optional(params, "spacing_m", request.spacingMeters));
            request.searchKilometres = static_cast<f32>(Optional(params, "search_km", request.searchKilometres));
            if (const Value* ice = params.Find("ice"); ice != nullptr)
            {
                if (!ice->IsBool()) { throw rpc::Error(kBadParams, "ice must be a boolean."); }
                request.ice = ice->AsBool();
            }
            if (const Value* nested = params.Find("nested"); nested != nullptr)
            {
                if (!nested->IsBool()) { throw rpc::Error(kBadParams, "nested must be a boolean."); }
                request.nested = nested->AsBool();
            }
            const std::string error = mailbox.SetAuto(request);
            Value::Object result{{"error", error}};
            for (auto& entry : StatusValue(mailbox.Status()).AsObject()) { result.insert(entry); }
            return result;
        });

    dispatcher.Register(
        {
            .name = "storm.reload_shaders",
            .description =
                "Recompiles the storm solver's compute kernels (engine/weather_gpu/shaders/storm.hlsl) without restarting: from `path`, or "
                "from the file named by ORBIT_WEATHER_GPU_SHADER, or the embedded copy when neither is given. The same thing happens "
                "automatically when storm.hlsl is saved while Studio runs (hot iteration). A kernel that does not compile leaves the "
                "running ones in use and reports the error in `shader_reload` of storm.status; the storm itself is untouched. The reload "
                "happens on the next frame that draws clouds.",
            .mutating = true
        },
        [&mailbox](const Value& params)
        {
            std::string path;
            if (const Value* p = params.IsObject() ? params.Find("path") : nullptr; p != nullptr)
            {
                if (!p->IsString()) { throw rpc::Error(kBadParams, "path must be a string."); }
                path = p->AsString();
            }
            mailbox.RequestShaderReload(path);
            return StatusValue(mailbox.Status());
        });

    dispatcher.Register(
        {
            .name = "storm.lightning",
            .description =
                "Lightning from the simulated storm's own graupel, ice, supercooled water and updraft (storm_electrification on a worker thread, "
                "flashes handed to the transient flash registry; clouds are lit by them when Studio runs with ORBIT_CLOUD_POINT_LIGHTS=1). "
                "Parameters (all optional, the rest keep their value): `enabled` (default true), and for testing `slow_motion` (1 to 2000: every "
                "flash lasts that many times longer at the same power, so one frame can catch it), `charge_boost` (1 to 10000: multiplies the "
                "charging rate) and `breakdown_scale` (0.02 to 1: scales the field at which air breaks down, lower = electrically unstable). "
                "Changing the boost or the breakdown restarts the charge. Returns `error`, the settings and `lightning_flashes` from storm.status.",
            .mutating = true
        },
        [&mailbox](const Value& params)
        {
            StormLightningSettings settings = mailbox.Lightning();
            if (params.IsObject())
            {
                if (const Value* enabled = params.Find("enabled"); enabled != nullptr)
                {
                    if (!enabled->IsBool()) { throw rpc::Error(kBadParams, "enabled must be a boolean."); }
                    settings.enabled = enabled->AsBool();
                }
                settings.slowMotion = Optional(params, "slow_motion", settings.slowMotion);
                settings.chargeBoost = Optional(params, "charge_boost", settings.chargeBoost);
                settings.breakdownScale = Optional(params, "breakdown_scale", settings.breakdownScale);
            }
            const std::string error = mailbox.SetLightning(settings);
            const StormLightningSettings now = mailbox.Lightning();
            Value::Object result{{"error", error},
                {"enabled", now.enabled},
                {"slow_motion", now.slowMotion},
                {"charge_boost", now.chargeBoost},
                {"breakdown_scale", now.breakdownScale},
                {"lightning_flashes", static_cast<f64>(mailbox.Status().lightningFlashes)}};
            return result;
        });

    dispatcher.Register(
        {
            .name = "storm.clear",
            .description = "Removes the simulated storm domain; the procedural clouds return.",
            .mutating = true
        },
        [&mailbox](const Value&)
        {
            mailbox.Clear();
            return StatusValue(mailbox.Status());
        });
}
} // namespace orbit::studio_ui

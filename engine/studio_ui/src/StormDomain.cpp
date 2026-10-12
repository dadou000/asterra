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
        {"lightning_ground_flashes", static_cast<f64>(s.lightningGroundFlashes)},
        {"lightning_arc_segments", static_cast<f64>(s.lightningArcSegments)},
        {"lightning_job_ms", s.lightningJobMilliseconds},
        {"lightning_flash_cells", static_cast<f64>(s.lightningFlashCells)},
        {"lightning_flash_radius_m", s.lightningFlashRadiusMeters},
        {"lightning_flash_radius_max_m", s.lightningFlashRadiusMaxMeters},
        {"scheduled_nearby", static_cast<f64>(s.scheduledNearby)},
        {"zone_count", static_cast<f64>(s.discoveredZones)},
        {"zone_candidate_probes", static_cast<f64>(s.zoneCandidateProbes)},
        {"zone_generation", static_cast<f64>(s.zoneGeneration)},
        {"zone_snapshot_seconds", s.zoneSnapshotSeconds},
        {"zone_survey_busy", s.zoneSurveyBusy},
        {"zone_work_tickets", static_cast<f64>(s.zoneWorkTickets)},
        {"zone_long_steps_blocked", static_cast<f64>(s.zoneBlockedLongSteps)},
        {"zone_tasks_deferred", static_cast<f64>(s.zoneDeferredTasks)},
        {"zone_max_lag_seconds", s.zoneMaximumLagSeconds},
        {"zone_completed_steps", static_cast<f64>(s.zoneCompletedSteps)},
        {"zone_pending_exchanges", static_cast<f64>(s.zonePendingExchanges)},
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
    {
        const std::scoped_lock lock(mutex_);
        place_ = request;
        manual_ = request;
        clear_ = false;
        status_ = {};
        status_.state = StormStatus::State::Pending;
        status_.message = "waiting for a frame that draws clouds on the body under the primary view";
        status_.latitudeDegrees = request.latitudeDegrees;
        status_.longitudeDegrees = request.longitudeDegrees;
        status_.cells = request.cells;
        status_.spacingMeters = request.spacingMeters;
    }
    NotifyChanged();
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
    {
        const std::scoped_lock lock(mutex_);
        auto_ = request;
        autoSetting_ = request;
    }
    NotifyChanged();
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
    if (!(settings.brightness >= 1.0 && settings.brightness <= 1.0e6)) { return "brightness must be 1 to 1e6"; }
    if (!(settings.flashSize >= 1.0 && settings.flashSize <= 50.0)) { return "flash_size must be 1 to 50"; }
    if (!(settings.groundStrikeChance >= 0.0 && settings.groundStrikeChance <= 1.0)) { return "ground_strike_chance must be 0 to 1"; }
    if (!(settings.smallFlashFraction >= 0.0 && settings.smallFlashFraction <= 1.0)) { return "small_flash_fraction must be 0 to 1"; }
    {
        const std::scoped_lock lock(mutex_);
        lightning_ = settings;
    }
    NotifyChanged();
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
    {
        const std::scoped_lock lock(mutex_);
        place_.reset();
        manual_.reset();
        clear_ = true;
        status_ = {};
    }
    NotifyChanged();
}

StormPersistentState StormDomainMailbox::Persistent() const
{
    const std::scoped_lock lock(mutex_);
    return {manual_, autoSetting_, lightning_};
}

void StormDomainMailbox::SetChangeHandler(std::function<void(const StormPersistentState&)> handler)
{
    const std::scoped_lock lock(mutex_);
    changed_ = std::move(handler);
}

void StormDomainMailbox::NotifyChanged()
{
    std::function<void(const StormPersistentState&)> handler;
    StormPersistentState state;
    {
        const std::scoped_lock lock(mutex_);
        handler = changed_;
        state = {manual_, autoSetting_, lightning_};
    }
    if (handler) { handler(state); }
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
                "charging rate), `breakdown_scale` (0.02 to 1: scales the field at which air breaks down, lower = electrically unstable), `brightness` "
                "(1 to 1e6: multiplies the energy of every flash, lights and arc alike), `arcs` (default true: the channel of a live flash is drawn as a glowing line) `pause_on_strike` (default false: the simulation pauses just before a flash that is in view starts, so it can be played in slow motion with the rate slider or time.set), `frame_strike` (default false: the camera turns to a flash about to start and backs off until its whole channel is in view) `small_flash_fraction` (0 to 1, default 0.75: the share of flashes that are small intracloud ones, a few tens of cells, cheap and never to the ground; changing it restarts the charge), `ground_strike_chance` (0 to 1, default 0.25: the share of flashes that are descending leaders seeking the ground, with a return stroke and later strokes; changing it restarts the charge), `flash_size` (1 to 50, default 4: multiplies the charge one flash may neutralise, so flashes grow larger; changing it restarts the charge) and `show_grid` (default false: draws the electrified window as a box with a grid and the charge in it, red positive and blue negative). "
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
                settings.brightness = Optional(params, "brightness", settings.brightness);
                settings.flashSize = Optional(params, "flash_size", settings.flashSize);
                settings.groundStrikeChance = Optional(params, "ground_strike_chance", settings.groundStrikeChance);
                settings.smallFlashFraction = Optional(params, "small_flash_fraction", settings.smallFlashFraction);
                if (const Value* frame = params.Find("frame_strike"); frame != nullptr)
                {
                    if (!frame->IsBool()) { throw rpc::Error(kBadParams, "frame_strike must be a boolean."); }
                    settings.frameStrike = frame->AsBool();
                }
                if (const Value* grid = params.Find("show_grid"); grid != nullptr)
                {
                    if (!grid->IsBool()) { throw rpc::Error(kBadParams, "show_grid must be a boolean."); }
                    settings.showGrid = grid->AsBool();
                }
                if (const Value* pause = params.Find("pause_on_strike"); pause != nullptr)
                {
                    if (!pause->IsBool()) { throw rpc::Error(kBadParams, "pause_on_strike must be a boolean."); }
                    settings.pauseOnStrike = pause->AsBool();
                }
                if (const Value* arcs = params.Find("arcs"); arcs != nullptr)
                {
                    if (!arcs->IsBool()) { throw rpc::Error(kBadParams, "arcs must be a boolean."); }
                    settings.arcs = arcs->AsBool();
                }
            }
            const std::string error = mailbox.SetLightning(settings);
            const StormLightningSettings now = mailbox.Lightning();
            Value::Object result{{"error", error},
                {"enabled", now.enabled},
                {"slow_motion", now.slowMotion},
                {"charge_boost", now.chargeBoost},
                {"breakdown_scale", now.breakdownScale},
                {"brightness", now.brightness},
                {"arcs", now.arcs},
                {"pause_on_strike", now.pauseOnStrike},
                {"frame_strike", now.frameStrike},
                {"show_grid", now.showGrid},
                {"flash_size", now.flashSize},
                {"ground_strike_chance", now.groundStrikeChance},
                {"small_flash_fraction", now.smallFlashFraction},
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

rpc::Value StormStateToValue(const StormPersistentState& state)
{
    Value::Object out;
    if (state.manual.has_value())
    {
        const StormPlaceRequest& m = *state.manual;
        out.insert({"manual", Value::Object{{"latitude_deg", m.latitudeDegrees}, {"longitude_deg", m.longitudeDegrees},
            {"cells", static_cast<f64>(m.cells)}, {"spacing_m", static_cast<f64>(m.spacingMeters)}, {"bubble_k", static_cast<f64>(m.bubbleKelvin)},
            {"spin_minutes", static_cast<f64>(m.spinMinutes)}, {"ice", m.ice}, {"sponge_cells", static_cast<f64>(m.spongeCells)},
            {"nested", m.nested}}});
    }
    const StormAutoRequest& a = state.autoRequest;
    out.insert({"auto", Value::Object{{"enabled", a.enabled}, {"seed", static_cast<f64>(a.seed)}, {"cells", static_cast<f64>(a.cells)},
        {"spacing_m", static_cast<f64>(a.spacingMeters)}, {"search_km", static_cast<f64>(a.searchKilometres)}, {"ice", a.ice}, {"nested", a.nested}}});
    const StormLightningSettings& l = state.lightning;
    out.insert({"lightning", Value::Object{{"enabled", l.enabled}, {"slow_motion", l.slowMotion}, {"charge_boost", l.chargeBoost},
        {"breakdown_scale", l.breakdownScale}, {"brightness", l.brightness}, {"arcs", l.arcs}, {"pause_on_strike", l.pauseOnStrike},
        {"frame_strike", l.frameStrike}, {"show_grid", l.showGrid}, {"flash_size", l.flashSize}, {"ground_strike_chance", l.groundStrikeChance}, {"small_flash_fraction", l.smallFlashFraction}}});
    return out;
}

std::string RestoreStormState(StormDomainMailbox& mailbox, const rpc::Value& saved)
{
    std::string problems;
    const auto number = [](const Value& object, const char* key, const f64 fallback) {
        const Value* v = object.IsObject() ? object.Find(key) : nullptr;
        return v != nullptr && v->IsNumber() ? v->AsNumber() : fallback;
    };
    const auto boolean = [](const Value& object, const char* key, const bool fallback) {
        const Value* v = object.IsObject() ? object.Find(key) : nullptr;
        return v != nullptr && v->IsBool() ? v->AsBool() : fallback;
    };
    if (!saved.IsObject()) { return "the saved storm state is not an object"; }
    if (const Value* l = saved.Find("lightning"); l != nullptr)
    {
        StormLightningSettings s;
        s.enabled = boolean(*l, "enabled", s.enabled);
        s.slowMotion = number(*l, "slow_motion", s.slowMotion);
        s.chargeBoost = number(*l, "charge_boost", s.chargeBoost);
        s.breakdownScale = number(*l, "breakdown_scale", s.breakdownScale);
        s.brightness = number(*l, "brightness", s.brightness);
        s.arcs = boolean(*l, "arcs", s.arcs);
        s.pauseOnStrike = boolean(*l, "pause_on_strike", s.pauseOnStrike);
        s.frameStrike = boolean(*l, "frame_strike", s.frameStrike);
        s.showGrid = boolean(*l, "show_grid", s.showGrid);
        s.flashSize = number(*l, "flash_size", s.flashSize);
        s.groundStrikeChance = number(*l, "ground_strike_chance", s.groundStrikeChance);
        s.smallFlashFraction = number(*l, "small_flash_fraction", s.smallFlashFraction);
        if (const std::string error = mailbox.SetLightning(s); !error.empty()) { problems += "lightning: " + error + "; "; }
    }
    if (const Value* a = saved.Find("auto"); a != nullptr && boolean(*a, "enabled", false))
    {
        StormAutoRequest r;
        r.enabled = true;
        r.seed = static_cast<u64>(number(*a, "seed", static_cast<f64>(r.seed)));
        r.cells = static_cast<u32>(number(*a, "cells", r.cells));
        r.spacingMeters = static_cast<f32>(number(*a, "spacing_m", r.spacingMeters));
        r.searchKilometres = static_cast<f32>(number(*a, "search_km", r.searchKilometres));
        r.ice = boolean(*a, "ice", r.ice);
        r.nested = boolean(*a, "nested", r.nested);
        if (const std::string error = mailbox.SetAuto(r); !error.empty()) { problems += "auto: " + error + "; "; }
    }
    if (const Value* m = saved.Find("manual"); m != nullptr)
    {
        StormPlaceRequest r;
        r.latitudeDegrees = number(*m, "latitude_deg", r.latitudeDegrees);
        r.longitudeDegrees = number(*m, "longitude_deg", r.longitudeDegrees);
        r.cells = static_cast<u32>(number(*m, "cells", r.cells));
        r.spacingMeters = static_cast<f32>(number(*m, "spacing_m", r.spacingMeters));
        r.bubbleKelvin = static_cast<f32>(number(*m, "bubble_k", r.bubbleKelvin));
        r.spinMinutes = static_cast<f32>(number(*m, "spin_minutes", r.spinMinutes));
        r.spongeCells = static_cast<u32>(number(*m, "sponge_cells", r.spongeCells));
        r.ice = boolean(*m, "ice", r.ice);
        r.nested = boolean(*m, "nested", r.nested);
        if (const std::string error = mailbox.Place(r); !error.empty()) { problems += "manual storm: " + error + "; "; }
    }
    return problems;
}
} // namespace orbit::studio_ui

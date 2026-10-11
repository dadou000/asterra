#include <orbit/studio_ui/StormDomain.hpp>

#include <cmath>

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
        {"message", s.message},
        {"latitude_deg", s.latitudeDegrees},
        {"longitude_deg", s.longitudeDegrees},
        {"storm_seconds", s.stormSeconds},
        {"steps", static_cast<f64>(s.steps)},
        {"step_ms", s.stepMilliseconds},
        {"cells", static_cast<f64>(s.cells)},
        {"spacing_m", static_cast<f64>(s.spacingMeters)},
        {"resident_mib", s.residentMiB}};
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
                "`bubble_k` (default 2), `spin_minutes` (0 to 240), `ice` (default true), `sponge_cells` (default 12). One domain at a "
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
            const std::string error = mailbox.Place(request);
            Value::Object result{{"error", error}};
            for (auto& entry : StatusValue(mailbox.Status()).AsObject()) { result.insert(entry); }
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

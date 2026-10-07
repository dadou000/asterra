#include <orbit/studio_ui/SimulationControlsUi.hpp>
#include <orbit/world_model/CelestialLightingService.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <format>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

namespace orbit::studio_ui
{
namespace
{
using rpc::Value;

struct StepPreset
{
    std::string_view label;
    f64 seconds;
};

constexpr std::array<StepPreset, 8> kStepPresets{{
    {"1 s", 1.0},
    {"10 s", 10.0},
    {"1 min", 60.0},
    {"10 min", 600.0},
    {"1 hour", 3600.0},
    {"6 hours", 21'600.0},
    {"1 day", 86'400.0},
    {"10 days", 864'000.0},
}};

struct RatePreset
{
    std::string_view label;
    f64 rate;
};

// The last entry stands for any other rate, edited next to the selector.
constexpr std::array<RatePreset, 8> kRatePresets{{
    {"0.1x", 0.1},
    {"1x", 1.0},
    {"10x", 10.0},
    {"60x", 60.0},
    {"600x", 600.0},
    {"3600x", 3600.0},
    {"86400x", 86'400.0},
    {"Custom", 0.0},
}};

constexpr i32 kCustomRateIndex = static_cast<i32>(kRatePresets.size()) - 1;

struct ActiveDay
{
    universe::BodyId body{};
    f64 periodSeconds{86'400.0};
    f64 phaseDegrees{0.0};
    i64 epochMicroseconds{0};
    i32 zoneOffsetHours{0};
    std::optional<f64> observerLongitudeDegrees;
    std::optional<f64> observerLatitudeRadians;
};

template <typename T>
[[nodiscard]] T ReadProperty(
    const scene::ObjectStore& objects,
    const scene::ObjectId object,
    const schema::PropertyId property,
    const T fallback)
{
    const auto value = objects.GetProperty(object, property);
    if (value.has_value())
    {
        if (const auto* typed = std::get_if<T>(&*value))
        {
            return *typed;
        }
    }
    return fallback;
}

[[nodiscard]] ActiveDay ResolveActiveDay(
    const studio_session::StudioSession& session)
{
    ActiveDay day;
    const auto active = session.ActiveBody().Active();
    if (!active.has_value() || !session.World().HasWorld())
    {
        return day;
    }

    const auto& objects = session.World().Objects();
    const auto body = active->semanticObject;
    day.body = active->body;
    day.periodSeconds = ReadProperty(
        objects,
        body,
        world_model::kBodyRotationPeriodSeconds,
        day.periodSeconds);
    day.phaseDegrees = ReadProperty(
        objects,
        body,
        world_model::kBodyRotationPhaseDegrees,
        day.phaseDegrees);

    for (const auto& child : objects.Children(body))
    {
        if (child.type != world_model::kRotationCapabilityType ||
            !ReadProperty(
                objects,
                child.id,
                world_model::kCapabilityEnabled,
                true))
        {
            continue;
        }
        const std::string model = ReadProperty(
            objects,
            child.id,
            world_model::kCapabilityModel,
            std::string{"Uniform Spin"});
        if (model == "Uniform Spin")
        {
            day.periodSeconds = ReadProperty(
                objects,
                child.id,
                world_model::kRotationPeriodSeconds,
                day.periodSeconds);
            day.phaseDegrees = ReadProperty(
                objects,
                child.id,
                world_model::kRotationPhaseDegrees,
                day.phaseDegrees);
            day.epochMicroseconds = ReadProperty(
                objects,
                child.id,
                world_model::kRotationEpochMicroseconds,
                day.epochMicroseconds);
        }
        else if (model == "Fixed")
        {
            day.periodSeconds = 0.0;
        }
        break;
    }

    if (!std::isfinite(day.periodSeconds) || day.periodSeconds <= 0.0)
    {
        day.periodSeconds = 86'400.0;
    }

    // The primary perspective viewport's observer is in body-fixed metres.
    // Geographic longitude is atan2(+Z, +X), with +Y as the north pole.
    if (const auto site = session.TerrainRuntime().ObserverSite("studio.primary");
        site.has_value() && site->body == active->body)
    {
        const auto& observer = site->observer.meters;
        const f64 horizontalSquared =
            observer.x * observer.x + observer.z * observer.z;
        if (std::isfinite(horizontalSquared) && horizontalSquared > 0.0)
        {
            const f64 longitudeDegrees =
                std::atan2(observer.z, observer.x) *
                180.0 / std::numbers::pi_v<f64>;
            day.observerLongitudeDegrees = longitudeDegrees;
            day.observerLatitudeRadians = std::atan2(
                observer.y, std::sqrt(horizontalSquared));
            day.zoneOffsetHours = static_cast<i32>(
                std::clamp(std::round(longitudeDegrees / 15.0),
                           -12.0, 12.0));
        }
    }
    return day;
}

[[nodiscard]] f64 WrapFraction(const f64 value)
{
    const f64 wrapped = std::fmod(value, 1.0);
    return wrapped < 0.0 ? wrapped + 1.0 : wrapped;
}

[[nodiscard]] f64 FallbackUtcFraction(
    const i64 timeMicroseconds, const ActiveDay& day)
{
    const f64 elapsedSeconds =
        (static_cast<f64>(timeMicroseconds) -
         static_cast<f64>(day.epochMicroseconds)) /
        1.0e6;
    return WrapFraction(
        elapsedSeconds / day.periodSeconds +
        day.phaseDegrees / 360.0);
}

struct DayClock
{
    f64 utcFraction{0.0};
    f64 localFraction{0.0};
    std::optional<f64> starDeclinationRadians;
    bool solar{false};
};

[[nodiscard]] DayClock ResolveDayClock(
    const studio_session::StudioSession& session,
    const ActiveDay& day,
    const i64 timeMicroseconds)
{
    DayClock clock;
    clock.utcFraction = FallbackUtcFraction(timeMicroseconds, day);
    if (day.body && session.World().HasWorld())
    {
        const world_model::CelestialLightingService lighting(
            session.World().Objects(), session.World().Universe());
        const auto direct = lighting.DominantDirectLightingAtBody(
            day.body, {.microsecondsFromEpoch = timeMicroseconds});
        if (direct.has_value())
        {
            const auto& direction = direct->receiverBodyFixedToEmitterMeters;
            const f64 horizontalSquared =
                direction.x * direction.x + direction.z * direction.z;
            if (std::isfinite(horizontalSquared) && horizontalSquared > 0.0)
            {
                const f64 starLongitude = std::atan2(direction.z, direction.x);
                clock.utcFraction = WrapFraction(
                    0.5 - starLongitude / (2.0 * std::numbers::pi_v<f64>));
                clock.starDeclinationRadians = std::atan2(
                    direction.y, std::sqrt(horizontalSquared));
                clock.solar = true;
            }
        }
    }
    clock.localFraction = WrapFraction(
        clock.utcFraction + static_cast<f64>(day.zoneOffsetHours) / 24.0);
    return clock;
}

[[nodiscard]] f64 SignedFraction(const f64 value)
{
    f64 wrapped = std::fmod(value, 1.0);
    if (wrapped > 0.5)
    {
        wrapped -= 1.0;
    }
    else if (wrapped < -0.5)
    {
        wrapped += 1.0;
    }
    return wrapped;
}

[[nodiscard]] std::optional<i64> TimeForLocalDelta(
    const studio_session::StudioSession& session,
    const ActiveDay& day,
    const i64 initialTime,
    const f64 fractionDelta)
{
    const f64 current = ResolveDayClock(session, day, initialTime).localFraction;
    const f64 target = WrapFraction(current + fractionDelta);
    f64 candidate = static_cast<f64>(initialTime) +
        fractionDelta * day.periodSeconds * 1.0e6;
    for (i32 iteration = 0; iteration < 4; ++iteration)
    {
        if (!std::isfinite(candidate) || std::fabs(candidate) >= 9.0e18)
        {
            return std::nullopt;
        }
        const auto candidateTime = static_cast<i64>(candidate);
        const f64 observed = ResolveDayClock(session, day, candidateTime).localFraction;
        const f64 error = SignedFraction(target - observed);
        if (std::fabs(error) < 1.0 / (86'400.0 * 10.0))
        {
            return candidateTime;
        }
        const f64 probeSeconds = std::max(1.0, day.periodSeconds / 24.0);
        const f64 probeTime = candidate + probeSeconds * 1.0e6;
        if (!std::isfinite(probeTime) || std::fabs(probeTime) >= 9.0e18)
        {
            return std::nullopt;
        }
        const f64 probed = ResolveDayClock(
            session, day, static_cast<i64>(probeTime)).localFraction;
        const f64 slope = SignedFraction(probed - observed) / probeSeconds;
        if (!std::isfinite(slope) || std::fabs(slope) < 1.0e-12)
        {
            break;
        }
        candidate += std::clamp(error / slope,
                                -day.periodSeconds,
                                day.periodSeconds) * 1.0e6;
    }
    return std::isfinite(candidate) && std::fabs(candidate) < 9.0e18
        ? std::optional(static_cast<i64>(candidate))
        : std::nullopt;
}

[[nodiscard]] std::string FormatZone(const ActiveDay& day)
{
    return std::format("{:+03}h", day.zoneOffsetHours);
}

[[nodiscard]] std::string FormatLocalTime(const f64 fraction)
{
    const auto seconds = static_cast<unsigned long long>(std::floor(
        WrapFraction(fraction) * 86'400.0));
    const auto hour = seconds / 3600ULL;
    const auto minute = (seconds / 60ULL) % 60ULL;
    const auto second = seconds % 60ULL;
    return std::format("{:02}:{:02}:{:02}", hour, minute, second);
}

[[nodiscard]] math::Float4 MixSkyColor(
    const math::Float4 a,
    const math::Float4 b,
    const f32 amount)
{
    return {
        a.x + (b.x - a.x) * amount,
        a.y + (b.y - a.y) * amount,
        a.z + (b.z - a.z) * amount,
        1.0F};
}

[[nodiscard]] math::Float4 SkyGradientColor(
    const f64 dayFraction,
    const ActiveDay& day,
    const DayClock& clock,
    const math::Float4 daylight)
{
    const f64 latitude = day.observerLatitudeRadians.value_or(0.0);
    const f64 declination = clock.starDeclinationRadians.value_or(0.0);
    const f64 longitudeRemainder = day.observerLongitudeDegrees.value_or(0.0) -
        static_cast<f64>(day.zoneOffsetHours) * 15.0;
    const f64 hourAngle = 2.0 * std::numbers::pi_v<f64> *
        (dayFraction - 0.5) +
        longitudeRemainder * std::numbers::pi_v<f64> / 180.0;
    const f64 sineElevation = std::sin(latitude) * std::sin(declination) +
        std::cos(latitude) * std::cos(declination) * std::cos(hourAngle);
    const f64 elevationDegrees = std::asin(std::clamp(
        sineElevation, -1.0, 1.0)) * 180.0 / std::numbers::pi_v<f64>;
    struct Stop
    {
        f64 elevationDegrees;
        math::Float4 color;
    };
    const std::array<Stop, 7> stops{{
        {-18.0, {0.025F, 0.035F, 0.080F, 1.0F}},
        {-9.0, {0.11F, 0.09F, 0.24F, 1.0F}},
        {-3.0, {0.37F, 0.19F, 0.42F, 1.0F}},
        {0.0, {0.90F, 0.41F, 0.29F, 1.0F}},
        {5.0, {0.98F, 0.67F, 0.46F, 1.0F}},
        {15.0, MixSkyColor({0.77F, 0.82F, 0.91F, 1.0F}, daylight, 0.35F)},
        {40.0, daylight},
    }};
    if (elevationDegrees <= stops.front().elevationDegrees)
    {
        return stops.front().color;
    }
    for (std::size_t index = 1; index < stops.size(); ++index)
    {
        if (elevationDegrees <= stops[index].elevationDegrees)
        {
            const f32 t = static_cast<f32>(
                (elevationDegrees - stops[index - 1].elevationDegrees) /
                (stops[index].elevationDegrees - stops[index - 1].elevationDegrees));
            return MixSkyColor(stops[index - 1].color, stops[index].color, t);
        }
    }
    return daylight;
}

[[nodiscard]] math::Float4 DaylightSkyColor(
    const studio_session::StudioSession& session)
{
    const auto active = session.ActiveBody().Active();
    if (!active.has_value() || !session.World().HasWorld())
    {
        return {0.18F, 0.43F, 0.85F, 1.0F};
    }
    const auto& objects = session.World().Objects();
    std::optional<math::Double3> scattering;
    for (const auto& child : objects.Children(active->semanticObject))
    {
        if (child.type == world_model::kAtmosphereCapabilityType &&
            ReadProperty(objects, child.id, world_model::kCapabilityEnabled, true))
        {
            scattering = ReadProperty(
                objects, child.id,
                world_model::kAtmosphereRayleighScatteringPerMeter,
                math::Double3{5.802e-6, 13.558e-6, 33.1e-6});
            break;
        }
    }
    if (!scattering.has_value())
    {
        return {0.16F, 0.20F, 0.27F, 1.0F};
    }
    const f64 maximum = std::max({scattering->x, scattering->y, scattering->z, 1.0e-12});
    return {
        static_cast<f32>(0.10 + 0.43 * scattering->x / maximum),
        static_cast<f32>(0.17 + 0.53 * scattering->y / maximum),
        static_cast<f32>(0.30 + 0.62 * scattering->z / maximum),
        1.0F};
}

[[nodiscard]] i32 StepPresetIndex(const f64 seconds, const i32 fallback)
{
    for (std::size_t index = 0; index < kStepPresets.size(); ++index)
    {
        if (kStepPresets[index].seconds == seconds)
        {
            return static_cast<i32>(index);
        }
    }
    return fallback;
}

[[nodiscard]] i32 RatePresetIndex(const f64 rate)
{
    // The custom slot is last and never matches.
    for (std::size_t index = 0; index + 1U < kRatePresets.size(); ++index)
    {
        if (kRatePresets[index].rate == rate)
        {
            return static_cast<i32>(index);
        }
    }
    return kCustomRateIndex;
}

[[nodiscard]] std::optional<f64> OptionalNumber(
    const Value::Object& object,
    const char* key)
{
    const auto found = object.find(key);
    if (found == object.end() || found->second.IsNull())
    {
        return std::nullopt;
    }
    if (!found->second.IsNumber() ||
        !std::isfinite(found->second.AsNumber()))
    {
        throw rpc::Error(
            -32602, std::string(key) + " must be a finite number.");
    }
    return found->second.AsNumber();
}

[[nodiscard]] Value StateToValue(
    const SimulationControls& controls,
    const studio_session::StudioSession& session)
{
    const i64 microseconds = controls.TimeMicroseconds();
    const ActiveDay day = ResolveActiveDay(session);
    const DayClock clock = ResolveDayClock(session, day, microseconds);
    return Value(Value::Object{
        {"playing", controls.Playing()},
        {"paused", !controls.Playing()},
        {"rate", controls.Rate()},
        {"step_seconds", controls.StepSeconds()},
        {"time_microseconds", microseconds},
        {"time_seconds", static_cast<f64>(microseconds) / 1.0e6},
        {"time_text", SimulationControls::FormatTime(microseconds)},
        {"utc_time", FormatLocalTime(clock.utcFraction)},
        {"utc_time_seconds", clock.utcFraction * 86'400.0},
        {"local_time", FormatLocalTime(clock.localFraction)},
        {"local_time_seconds", clock.localFraction * 86'400.0},
        {"solar_time", clock.solar},
        {"local_day_seconds", day.periodSeconds},
        {"local_time_zone_hours", day.zoneOffsetHours},
        {"observer_longitude_degrees",
         day.observerLongitudeDegrees.has_value()
             ? Value(*day.observerLongitudeDegrees)
             : Value{}}});
}
} // namespace

SimulationControls::SimulationControls(
    studio_session::SimulationClock& clock) noexcept
    : clock_(&clock)
{
}

bool SimulationControls::Playing() const noexcept
{
    return clock_->Playing();
}

void SimulationControls::SetPlaying(const bool playing)
{
    clock_->SetPlaying(playing);
}

void SimulationControls::TogglePlaying()
{
    clock_->SetPlaying(!clock_->Playing());
}

f64 SimulationControls::Rate() const noexcept
{
    return clock_->Rate();
}

void SimulationControls::SetRate(const f64 simulationSecondsPerRealSecond)
{
    clock_->SetRate(simulationSecondsPerRealSecond);
}

i64 SimulationControls::TimeMicroseconds() const noexcept
{
    return clock_->Time().microsecondsFromEpoch;
}

void SimulationControls::SetTimeMicroseconds(const i64 microseconds)
{
    clock_->SetTime({.microsecondsFromEpoch = microseconds});
}

void SimulationControls::Step(const f64 simulationSeconds)
{
    clock_->StepSeconds(simulationSeconds);
}

f64 SimulationControls::StepSeconds() const noexcept
{
    return stepSeconds_;
}

void SimulationControls::SetStepSeconds(const f64 simulationSeconds)
{
    if (!std::isfinite(simulationSeconds) || simulationSeconds <= 0.0)
    {
        throw std::invalid_argument(
            "Simulation step must be a positive, finite number of seconds.");
    }
    stepSeconds_ = simulationSeconds;
}

std::string SimulationControls::FormatTime(const i64 microseconds)
{
    const bool negative = microseconds < 0;
    // Avoids negating INT64_MIN.
    const unsigned long long magnitude = negative
        ? static_cast<unsigned long long>(-(microseconds + 1)) + 1ULL
        : static_cast<unsigned long long>(microseconds);

    const unsigned long long totalMilliseconds = magnitude / 1000ULL;
    const unsigned long long milliseconds = totalMilliseconds % 1000ULL;
    const unsigned long long totalSeconds = totalMilliseconds / 1000ULL;
    const unsigned long long seconds = totalSeconds % 60ULL;
    const unsigned long long minutes = (totalSeconds / 60ULL) % 60ULL;
    const unsigned long long hours = (totalSeconds / 3600ULL) % 24ULL;
    const unsigned long long days = totalSeconds / 86'400ULL;

    return std::format(
        "T{}{}d {:02}:{:02}:{:02}.{:03}",
        negative ? '-' : '+',
        days,
        hours,
        minutes,
        seconds,
        milliseconds);
}

SimulationControlsUi::SimulationControlsUi(
    SimulationControls& controls,
    studio_session::StudioSession& session) noexcept
    : controls_(&controls),
      session_(&session)
{
}

SimulationControlsUi::~SimulationControlsUi()
{
    if (registered_)
    {
        static_cast<void>(editor_ui::RemoveShellBand(kBandId));
    }
}

void SimulationControlsUi::SetReportIssueHandler(std::function<void()> handler)
{
    reportIssue_ = std::move(handler);
}

void SimulationControlsUi::Register(editor_ui::EditorUi&)
{
    registered_ = true;
    editor_ui::UpsertShellBand({
        .id = kBandId,
        .order = 1,
        .height = 40.0F,
        .edge = editor_ui::ShellBandEdge::Bottom,
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                Draw(context);
            }});
}

void SimulationControlsUi::Draw(editor_ui::PanelContext& context)
{
    const bool playing = controls_->Playing();
    const f32 uiScale = editor_ui::CurrentUiScale();
    if (context.StateButton(
            playing ? "Pause##sim-toggle" : "Play##sim-toggle",
            playing
                ? math::Float4{0.16F, 0.48F, 0.27F, 1.0F}
                : math::Float4{0.30F, 0.33F, 0.39F, 1.0F},
            68.0F * uiScale))
    {
        controls_->TogglePlaying();
    }

    const f64 step = controls_->StepSeconds();

    context.SameLine();
    if (context.Button("<< Step##sim-step-back"))
    {
        controls_->Step(-step);
    }
    context.SameLine();
    if (context.Button("Step >>##sim-step-forward"))
    {
        controls_->Step(step);
    }

    context.SameLine();
    std::array<std::string_view, kStepPresets.size()> stepLabels{};
    for (std::size_t index = 0; index < kStepPresets.size(); ++index)
    {
        stepLabels[index] = kStepPresets[index].label;
    }
    stepIndex_ = StepPresetIndex(step, stepIndex_);
    if (context.Combo(
            "##sim-step-size",
            stepLabels,
            stepIndex_,
            112.0F * uiScale))
    {
        controls_->SetStepSeconds(
            kStepPresets[static_cast<std::size_t>(stepIndex_)].seconds);
    }

    context.SameLine();
    std::array<std::string_view, kRatePresets.size()> rateLabels{};
    for (std::size_t index = 0; index < kRatePresets.size(); ++index)
    {
        rateLabels[index] = kRatePresets[index].label;
    }
    const f64 rate = controls_->Rate();
    const i32 matched = RatePresetIndex(rate);
    // A rate set from elsewhere (RPC) shows as its preset or as Custom; a
    // custom value being typed stays on Custom even if it passes a preset.
    if (!(rateIndex_ == kCustomRateIndex && rate == customRate_))
    {
        rateIndex_ = matched;
    }
    if (context.Combo(
            "##sim-rate",
            rateLabels,
            rateIndex_,
            104.0F * uiScale))
    {
        if (rateIndex_ == kCustomRateIndex)
        {
            customRate_ = rate;
        }
        else
        {
            controls_->SetRate(
                kRatePresets[static_cast<std::size_t>(rateIndex_)].rate);
        }
    }
    if (rateIndex_ == kCustomRateIndex)
    {
        context.SameLine();
        f64 edited = rate;
        if (context.InputDouble(
                "x##sim-rate-custom",
                edited,
                96.0F * uiScale))
        {
            try
            {
                controls_->SetRate(edited);
                customRate_ = edited;
            }
            catch (const std::exception&)
            {
                // A non-finite value is ignored; the clock keeps its rate.
            }
        }
    }

    const ActiveDay day = ResolveActiveDay(*session_);
    const DayClock clock = ResolveDayClock(
        *session_, day, controls_->TimeMicroseconds());
    const f64 fraction = clock.localFraction;
    context.SameLine();
    context.MutedText("UTC");
    context.SameLine();
    context.Text(FormatLocalTime(clock.utcFraction));
    context.SameLine();
    context.MutedText("Local");
    context.SameLine();
    context.Text(FormatLocalTime(fraction));
    if (day.observerLongitudeDegrees.has_value())
    {
        context.SameLine();
        context.MutedText(FormatZone(day));
    }

    context.SameLine();
    const f32 reservedWidth = reportIssue_ ? 112.0F * uiScale : 0.0F;
    const f32 sliderWidth = std::max(
        160.0F * uiScale,
        context.ContentAvailable().width - reservedWidth);
    const auto scrubber = context.Canvas(
        "##sim-local-day-scrubber",
        {sliderWidth, 26.0F * uiScale});
    const math::Float4 daylight = DaylightSkyColor(*session_);
    context.CanvasRect(
        {0.02F, 0.24F},
        {0.98F, 0.76F},
        {0.12F, 0.15F, 0.22F, 1.0F},
        true);
    constexpr i32 kSkyGradientSegments = 96;
    for (i32 index = 0; index < kSkyGradientSegments; ++index)
    {
        const f32 left = static_cast<f32>(index) /
            static_cast<f32>(kSkyGradientSegments);
        const f32 right = static_cast<f32>(index + 1) /
            static_cast<f32>(kSkyGradientSegments);
        context.CanvasGradientRect(
            {0.02F + 0.96F * left, 0.28F},
            {0.02F + 0.96F * right, 0.72F},
            SkyGradientColor(left, day, clock, daylight),
            SkyGradientColor(right, day, clock, daylight));
    }
    context.CanvasRect(
        {0.02F, 0.28F},
        {0.02F + 0.96F * static_cast<f32>(fraction), 0.72F},
        {1.0F, 1.0F, 1.0F, 0.10F},
        true);
    context.CanvasCircle(
        {0.02F + 0.96F * static_cast<f32>(fraction), 0.50F},
        9.0F * uiScale,
        {0.07F, 0.09F, 0.14F, 1.0F});
    context.CanvasCircle(
        {0.02F + 0.96F * static_cast<f32>(fraction), 0.50F},
        7.0F * uiScale,
        {0.94F, 0.97F, 1.0F, 1.0F});
    if (scrubber.hovered)
    {
        context.CanvasTooltip(
            day.observerLongitudeDegrees.has_value()
                ? std::format(
                      "{} clock; zone {} at {:.1f} deg longitude. Drag across midnight to keep scrubbing.",
                      clock.solar ? "Dominant-star solar" : "Rotation fallback",
                      FormatZone(day),
                      *day.observerLongitudeDegrees)
                : "Prime-meridian clock. Drag across midnight to keep scrubbing.");
    }

    if (scrubber.clicked)
    {
        f64 fractionDelta =
            static_cast<f64>(scrubber.u - 0.02F) / 0.96 - fraction;
        if (fractionDelta > 0.5)
        {
            fractionDelta -= 1.0;
        }
        else if (fractionDelta < -0.5)
        {
            fractionDelta += 1.0;
        }
        if (const auto newTime = TimeForLocalDelta(
                *session_, day, controls_->TimeMicroseconds(), fractionDelta))
        {
            controls_->SetTimeMicroseconds(*newTime);
        }
    }
    else if (std::fabs(scrubber.dragDeltaX) > 0.0F && sliderWidth > 0.0F)
    {
        const f64 fractionDelta =
            static_cast<f64>(scrubber.dragDeltaX) /
            (static_cast<f64>(sliderWidth) * 0.96);
        if (const auto newTime = TimeForLocalDelta(
                *session_, day, controls_->TimeMicroseconds(), fractionDelta))
        {
            controls_->SetTimeMicroseconds(*newTime);
        }
    }

    if (reportIssue_)
    {
        context.SameLine();
        if (context.Button("Report issue##sim-report"))
        {
            reportIssue_();
        }
    }
}

void RegisterSimulationRpc(
    rpc::Dispatcher& dispatcher,
    SimulationControls& controls,
    studio_session::StudioSession& session)
{
    dispatcher.Register(
        {
            .name = "time.get",
            .description =
                "The Studio simulation clock that drives planetary rotation, "
                "orbits, the sun and the atmosphere/weather: playing or "
                "paused, rate (simulation seconds per real second), time "
                "since the epoch (microseconds, seconds and as text), "
                "planetary UTC, dominant-star local solar time, the "
                "active-viewport longitude zone, and the "
                "step size of the Step buttons.",
            .mutating = false
        },
        [&controls, &session](const Value&)
        {
            return StateToValue(controls, session);
        });

    dispatcher.Register(
        {
            .name = "time.set",
            .description =
                "Drives the simulation transport. Fields (all optional): "
                "playing (true = Simulate, false = Pause), rate (simulation "
                "seconds per real second, negative runs backwards), "
                "time_microseconds (jump to an absolute time since the "
                "epoch), local_time_seconds (active body's current viewpoint "
                "solar zone clock time in [0, 86400)), step_seconds (size of the "
                "Step buttons). Returns "
                "the new state.",
            .mutating = true
        },
        [&controls, &session](const Value& params)
        {
            if (!params.IsObject())
            {
                return StateToValue(controls, session);
            }
            const auto& object = params.AsObject();

            try
            {
                if (const auto rate = OptionalNumber(object, "rate"))
                {
                    controls.SetRate(*rate);
                }
                if (const auto step = OptionalNumber(object, "step_seconds"))
                {
                    controls.SetStepSeconds(*step);
                }
                if (const auto time = OptionalNumber(object, "time_microseconds"))
                {
                    if (std::fabs(*time) >= 9.0e18)
                    {
                        throw rpc::Error(
                            -32602, "time_microseconds is out of range.");
                    }
                    controls.SetTimeMicroseconds(static_cast<i64>(*time));
                }
                if (const auto localTime = OptionalNumber(object, "local_time_seconds"))
                {
                    if (*localTime < 0.0 || *localTime >= 86'400.0)
                    {
                        throw rpc::Error(
                            -32602,
                            "local_time_seconds must be in [0, 86400).");
                    }
                    const ActiveDay day = ResolveActiveDay(session);
                    f64 fractionDelta = *localTime / 86'400.0 -
                        ResolveDayClock(session, day, controls.TimeMicroseconds())
                            .localFraction;
                    if (fractionDelta > 0.5)
                    {
                        fractionDelta -= 1.0;
                    }
                    else if (fractionDelta < -0.5)
                    {
                        fractionDelta += 1.0;
                    }
                    const auto newTime = TimeForLocalDelta(
                        session, day, controls.TimeMicroseconds(), fractionDelta);
                    if (!newTime.has_value())
                    {
                        throw rpc::Error(-32602, "local_time_seconds is out of range.");
                    }
                    controls.SetTimeMicroseconds(*newTime);
                }
                if (const auto found = object.find("playing");
                    found != object.end() && !found->second.IsNull())
                {
                    if (!found->second.IsBool())
                    {
                        throw rpc::Error(
                            -32602, "playing must be a boolean.");
                    }
                    controls.SetPlaying(found->second.AsBool());
                }
            }
            catch (const rpc::Error&)
            {
                throw;
            }
            catch (const std::exception& exception)
            {
                throw rpc::Error(1090, exception.what());
            }
            return StateToValue(controls, session);
        });

    dispatcher.Register(
        {
            .name = "time.step",
            .description =
                "Advances the simulation clock by seconds (negative "
                "rewinds), playing or paused: the Step buttons. seconds "
                "defaults to the configured step size. Returns the new "
                "state.",
            .mutating = true
        },
        [&controls, &session](const Value& params)
        {
            f64 seconds = controls.StepSeconds();
            if (params.IsObject())
            {
                if (const auto given = OptionalNumber(params.AsObject(), "seconds"))
                {
                    seconds = *given;
                }
            }
            try
            {
                controls.Step(seconds);
            }
            catch (const std::exception& exception)
            {
                throw rpc::Error(1090, exception.what());
            }
            return StateToValue(controls, session);
        });
}
} // namespace orbit::studio_ui

#include <orbit/studio_ui/SimulationControlsUi.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <format>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

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
    {"1x (real time)", 1.0},
    {"10x", 10.0},
    {"60x (1 min/s)", 60.0},
    {"600x (10 min/s)", 600.0},
    {"3600x (1 h/s)", 3600.0},
    {"86400x (1 day/s)", 86'400.0},
    {"Custom", 0.0},
}};

constexpr i32 kCustomRateIndex = static_cast<i32>(kRatePresets.size()) - 1;

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

[[nodiscard]] Value StateToValue(const SimulationControls& controls)
{
    const i64 microseconds = controls.TimeMicroseconds();
    return Value(Value::Object{
        {"playing", controls.Playing()},
        {"paused", !controls.Playing()},
        {"rate", controls.Rate()},
        {"step_seconds", controls.StepSeconds()},
        {"time_microseconds", microseconds},
        {"time_seconds", static_cast<f64>(microseconds) / 1.0e6},
        {"time_text", SimulationControls::FormatTime(microseconds)}});
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
    SimulationControls& controls) noexcept
    : controls_(&controls)
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

    if (playing
            ? context.Button("Pause##sim-toggle")
            : context.PrimaryButton("Simulate##sim-toggle"))
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
    if (context.Combo("##sim-step-size", stepLabels, stepIndex_))
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
    if (context.Combo("##sim-rate", rateLabels, rateIndex_))
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
        if (context.InputDouble("x##sim-rate-custom", edited))
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

    context.SameLine();
    context.Text(SimulationControls::FormatTime(controls_->TimeMicroseconds()));

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
    SimulationControls& controls)
{
    dispatcher.Register(
        {
            .name = "time.get",
            .description =
                "The Studio simulation clock that drives planetary rotation, "
                "orbits, the sun and the atmosphere/weather: playing or "
                "paused, rate (simulation seconds per real second), time "
                "since the epoch (microseconds, seconds and as text) and the "
                "step size of the Step buttons.",
            .mutating = false
        },
        [&controls](const Value&)
        {
            return StateToValue(controls);
        });

    dispatcher.Register(
        {
            .name = "time.set",
            .description =
                "Drives the simulation transport. Fields (all optional): "
                "playing (true = Simulate, false = Pause), rate (simulation "
                "seconds per real second, negative runs backwards), "
                "time_microseconds (jump to an absolute time since the "
                "epoch), step_seconds (size of the Step buttons). Returns "
                "the new state.",
            .mutating = true
        },
        [&controls](const Value& params)
        {
            if (!params.IsObject())
            {
                return StateToValue(controls);
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
            return StateToValue(controls);
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
        [&controls](const Value& params)
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
            return StateToValue(controls);
        });
}
} // namespace orbit::studio_ui

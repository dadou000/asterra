#include <orbit/studio_ui/DisplayEyeRpc.hpp>

#include <cmath>
#include <string>
#include <utility>

namespace orbit::studio_ui
{
namespace
{
using rpc::Value;
using post_process::HumanEyeAdaptationConfig;

constexpr i64 kInvalid = 1072;

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

struct NumberField
{
    const char* name;
    f32 HumanEyeAdaptationConfig::* member;
    f32 minimum;
    f32 maximum;
};

// Every numeric control of the panel, named like its display.eye.* setting.
constexpr NumberField kNumberFields[] = {
    {"nits_per_scene_unit", &HumanEyeAdaptationConfig::nitsPerSceneUnit, 1.0e-3F, 1.0e9F},
    {"glare_threshold_nits", &HumanEyeAdaptationConfig::glareThresholdNits, 1.0F, 1.0e12F},
    {"highlight_attack_seconds", &HumanEyeAdaptationConfig::highlightAttackSeconds, 0.0F, 60.0F},
    {"daylight_adaptation_nits", &HumanEyeAdaptationConfig::daylightAdaptationNits, 1.0e-3F, 1.0e9F},
    {"max_boost_stops", &HumanEyeAdaptationConfig::maximumBoostStops, 0.0F, 40.0F},
    {"photopic_ceiling_log2", &HumanEyeAdaptationConfig::photopicCeilingLog2, -64.0F, 64.0F},
    {"exposure_middle_gray", &HumanEyeAdaptationConfig::exposureMiddleGray, 1.0e-4F, 1.0F},
    {"min_exposure_scale", &HumanEyeAdaptationConfig::minimumExposureScale, 1.0e-8F, 1.0e8F},
    {"max_exposure_scale", &HumanEyeAdaptationConfig::maximumExposureScale, 1.0e-8F, 1.0e8F},
    {"photopic_brighten_seconds", &HumanEyeAdaptationConfig::photopicBrightenSeconds, 0.0F, 600.0F},
    {"photopic_darken_seconds", &HumanEyeAdaptationConfig::photopicDarkenSeconds, 0.0F, 600.0F},
    {"dark_adapt_seconds", &HumanEyeAdaptationConfig::darkAdaptSeconds, 0.0F, 3600.0F},
    {"overload_attack_seconds", &HumanEyeAdaptationConfig::overloadAttackSeconds, 0.0F, 60.0F},
    {"overload_recovery_seconds", &HumanEyeAdaptationConfig::overloadRecoverySeconds, 0.0F, 60.0F},
};

[[nodiscard]] Value Report(
    const std::string& id,
    const StudioLuminanceHistogramDiagnostics& d)
{
    const auto& c = d.eyeConfig;
    const auto& s = d.eyeState;
    Value::Object config{
        {"highlight_protection", c.highlightProtection},
        // Copied from the tone-mapping config every frame; read-only here.
        {"reference_white_nits", static_cast<f64>(d.toneMapping.referenceWhiteNits)},
        {"display_peak_nits", static_cast<f64>(d.toneMapping.peakNits)}};
    for (const NumberField& field : kNumberFields)
    {
        config.emplace(field.name, static_cast<f64>(c.*field.member));
    }

    return Value(Value::Object{
        {"id", id},
        {"config", Value(std::move(config))},
        {"state", Value(Value::Object{
            {"initialized", s.initialized},
            {"exposure_scale", static_cast<f64>(s.exposureScale)},
            {"target_exposure_scale", static_cast<f64>(s.targetExposureScale)},
            {"brightest_pixel_nits", static_cast<f64>(s.peakNits)},
            {"adapted_nits",
             std::exp2(static_cast<f64>(s.exposureAdaptationLog2)) *
                 static_cast<f64>(c.nitsPerSceneUnit)},
            {"highlight_protection_stops", static_cast<f64>(s.highlightProtectionStops)},
            {"boost_limit_stops", static_cast<f64>(s.boostLimitStops)},
            {"photopic_log2", static_cast<f64>(s.photopicLog2)},
            {"dark_adaptation", static_cast<f64>(s.darkAdaptation)},
            {"overload", static_cast<f64>(s.overload)}})}});
}
} // namespace

void RegisterDisplayEyeRpc(
    rpc::Dispatcher& dispatcher,
    StudioViewportRenderer& renderer)
{
    dispatcher.Register(
        {
            .name = "display.eye_get",
            .description =
                "Eye adaptation (auto-exposure) of a Studio view, in cd/m2: "
                "the config (highlight_protection, glare_threshold_nits, "
                "daylight_adaptation_nits, max_boost_stops, ...) and the live "
                "state (exposure_scale, brightest_pixel_nits, adapted_nits, "
                "highlight_protection_stops, boost_limit_stops). One scene "
                "unit is 929563 cd/m2 (nits_per_scene_unit). id defaults to "
                "studio.primary.",
            .mutating = false
        },
        [&renderer](const Value& params)
        {
            const std::string id = ViewIdOrPrimary(params);
            const auto diagnostics = renderer.LuminanceHistogramDiagnostics(id);
            if (!diagnostics.has_value())
            {
                throw rpc::Error(kInvalid, "View '" + id + "' has no luminance metering yet.");
            }
            return Report(id, *diagnostics);
        });

    dispatcher.Register(
        {
            .name = "display.eye_set",
            .description =
                "Changes eye adaptation settings of a Studio view; only the "
                "given fields change (highlight_protection, "
                "glare_threshold_nits, highlight_attack_seconds, "
                "daylight_adaptation_nits, max_boost_stops, "
                "nits_per_scene_unit, photopic_*/dark_*/overload_* ...; see "
                "display.eye_get). Values outside the valid range are "
                "rejected. Returns the resulting config and state. Not "
                "persisted to the display settings file.",
            .mutating = true
        },
        [&renderer](const Value& params)
        {
            const std::string id = ViewIdOrPrimary(params);
            const auto diagnostics = renderer.LuminanceHistogramDiagnostics(id);
            if (!diagnostics.has_value() || !params.IsObject())
            {
                throw rpc::Error(kInvalid, "View '" + id + "' has no luminance metering yet.");
            }

            HumanEyeAdaptationConfig config = diagnostics->eyeConfig;
            const auto& object = params.AsObject();

            if (const auto found = object.find("highlight_protection");
                found != object.end())
            {
                if (!found->second.IsBool())
                {
                    throw rpc::Error(-32602, "highlight_protection must be a boolean.");
                }
                config.highlightProtection = found->second.AsBool();
            }
            for (const NumberField& field : kNumberFields)
            {
                const auto found = object.find(field.name);
                if (found == object.end())
                {
                    continue;
                }
                if (!found->second.IsNumber() ||
                    !std::isfinite(found->second.AsNumber()) ||
                    found->second.AsNumber() < field.minimum ||
                    found->second.AsNumber() > field.maximum)
                {
                    throw rpc::Error(
                        -32602,
                        std::string(field.name) + " must be a number in [" +
                            std::to_string(field.minimum) + ", " +
                            std::to_string(field.maximum) + "].");
                }
                config.*field.member = static_cast<f32>(found->second.AsNumber());
            }

            renderer.SetHumanEyeAdaptationConfig(id, config);
            return Report(id, *renderer.LuminanceHistogramDiagnostics(id));
        });

    dispatcher.Register(
        {
            .name = "display.eye_reset",
            .description =
                "Restarts the eye adaptation state of a Studio view (as after "
                "a cut): the next frame adapts instantly instead of easing. "
                "id defaults to studio.primary.",
            .mutating = true
        },
        [&renderer](const Value& params)
        {
            const std::string id = ViewIdOrPrimary(params);
            renderer.ResetHumanEyeAdaptation(id);
            return Value(Value::Object{{"id", id}, {"reset", true}});
        });
}
} // namespace orbit::studio_ui

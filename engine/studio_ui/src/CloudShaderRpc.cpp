#include <orbit/studio_ui/CloudShaderRpc.hpp>

#include <chrono>
#include <string>

namespace orbit::studio_ui
{
namespace
{
using rpc::Value;
constexpr i64 kBadParams = -32602;
constexpr std::size_t kMaxDefinesLength = 2048;
} // namespace

void RegisterCloudShaderRpc(rpc::Dispatcher& dispatcher, CloudShaderHost host)
{
    dispatcher.Register(
        {
            .name = "clouds.get_defines",
            .description = "The cloud shader defines in use, as \"NAME=VALUE;NAME=VALUE\" (empty = the shipped shaders).",
            .mutating = false
        },
        [host](const Value&) { return Value::Object{{"defines", host.get()}}; });

    dispatcher.Register(
        {
            .name = "clouds.set_defines",
            .description =
                "Recompiles the cloud march, shadow and volume shaders with `defines` (\"NAME=VALUE;NAME=VALUE\", an empty "
                "string restores the shipped shaders) so a march tunable can be changed without relaunching. Tunables: "
                "CLOUD_FAR_EARLYOUT, CLOUD_JITTER_FIX, CLOUD_INCLOUD_STEP_SCALE, CLOUD_MAX_STEPS, CLOUD_SCHEDULE_STEPS, "
                "CLOUD_FOOTPRINT_STEP, CLOUD_BASE_STEP, CLOUD_EMPTY_SKIP, CLOUD_EMPTY_COVERAGE, CLOUD_SUN_FAR_START, "
                "CLOUD_SUN_FAR_END, CLOUD_SUN_VOLUME, CLOUD_RESOLVE_WEIGHT (the temporal resolve's current-frame weight, 0 to 1; "
                "unset = 0.2) and the CLOUD_PROF_* profiling stubs. Returns `defines` in use, `compile_ms` and `error` (empty on success; on a compile error "
                "the previous shaders stay in use). Give the image a second or two to converge after a change (temporal "
                "history and the light volume refresh).",
            .mutating = true
        },
        [host](const Value& params)
        {
            const Value* defines = params.IsObject() ? params.Find("defines") : nullptr;
            if (defines == nullptr || !defines->IsString())
            {
                throw rpc::Error(kBadParams, "defines must be a string (empty restores the shipped shaders).");
            }
            const std::string& text = defines->AsString();
            if (text.size() > kMaxDefinesLength)
            {
                throw rpc::Error(kBadParams, "defines is too long.");
            }
            const auto start = std::chrono::steady_clock::now();
            const std::string error = host.set(text);
            const f64 milliseconds =
                std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
            return Value::Object{{"defines", host.get()}, {"compile_ms", milliseconds}, {"error", error}};
        });

    if (!host.getSplit || !host.setSplit)
    {
        return;
    }
    dispatcher.Register(
        {
            .name = "clouds.split_get",
            .description = "The cloud split view: `enabled`, `defines` (the right half's, \"NAME=VALUE;...\") and `position` (0..1).",
            .mutating = false
        },
        [host](const Value&)
        {
            const CloudSplitState state = host.getSplit();
            return Value::Object{{"enabled", state.enabled}, {"defines", state.defines}, {"position", static_cast<f64>(state.position)}};
        });

    dispatcher.Register(
        {
            .name = "clouds.split_set",
            .description =
                "Split-screen view of the cloud march: the left of the viewport renders with the defines in use (clouds.set_defines), "
                "the right with `defines`, divided at `position` (0..1 of the width, default 0.5, a white line marks it). Each half is "
                "marched and resolved on its own, so the Clouds GPU time is that of the two halves. Only the march and the temporal "
                "resolve take `defines` (CLOUD_CHECKERBOARD, CLOUD_CHECKERBOARD_WEIGHT, CLOUD_RESOLVE_WEIGHT, CLOUD_MAX_STEPS, ...); the "
                "shadow and light-volume shaders stay as they are. `enabled` false returns to the single view. Returns `enabled`, "
                "`defines`, `position` and `error` (empty on success; on a compile error the split stays as it was).",
            .mutating = true
        },
        [host](const Value& params)
        {
            const Value* enabled = params.IsObject() ? params.Find("enabled") : nullptr;
            if (enabled == nullptr || !enabled->IsBool())
            {
                throw rpc::Error(kBadParams, "enabled must be a boolean.");
            }
            const Value* defines = params.Find("defines");
            std::string text;
            if (defines != nullptr)
            {
                if (!defines->IsString())
                {
                    throw rpc::Error(kBadParams, "defines must be a string.");
                }
                text = defines->AsString();
                if (text.size() > kMaxDefinesLength)
                {
                    throw rpc::Error(kBadParams, "defines is too long.");
                }
            }
            const Value* position = params.Find("position");
            f32 divider = host.getSplit().position;
            if (position != nullptr)
            {
                if (!position->IsNumber() || position->AsNumber() < 0.0 || position->AsNumber() > 1.0)
                {
                    throw rpc::Error(kBadParams, "position must be a number between 0 and 1.");
                }
                divider = static_cast<f32>(position->AsNumber());
            }
            const std::string error = host.setSplit(enabled->AsBool(), text, divider);
            const CloudSplitState state = host.getSplit();
            return Value::Object{
                {"enabled", state.enabled}, {"defines", state.defines}, {"position", static_cast<f64>(state.position)}, {"error", error}};
        });
}
} // namespace orbit::studio_ui

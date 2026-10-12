#include <orbit/studio_ui/SessionState.hpp>

#include <orbit/core/Log.hpp>

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace orbit::studio_ui
{
namespace
{
using rpc::Value;
constexpr f64 kRestoreGiveUpSeconds = 90.0;
constexpr f64 kRestoreIntervalSeconds = 0.5;
constexpr f64 kSaveIntervalSeconds = 5.0;
constexpr const char* kPrimaryView = "studio.primary";

// The clock fields worth keeping, from the result of time.get (the session state is saved with these).
Value::Object ClockFields(const Value& time)
{
    Value::Object fields;
    for (const char* key : {"time_microseconds", "rate", "playing"})
    {
        if (const Value* v = time.Find(key); v != nullptr) { fields.insert_or_assign(key, *v); }
    }
    return fields;
}
} // namespace

SessionStateKeeper::SessionStateKeeper(rpc::Dispatcher& dispatcher, std::filesystem::path file)
    : dispatcher_(&dispatcher), file_(std::move(file))
{
}

Value SessionStateKeeper::Call(const char* method, Value params) const
{
    const Value request = Value::Object{{"jsonrpc", "2.0"}, {"id", 1.0}, {"method", std::string(method)}, {"params", std::move(params)}};
    const std::optional<std::string> reply = dispatcher_->Dispatch(rpc::Serialize(request));
    if (!reply.has_value()) { throw std::runtime_error("no reply"); }
    const Value parsed = rpc::ParseValue(*reply);
    if (parsed.Find("error") != nullptr) { throw std::runtime_error(std::string(method) + " failed"); }
    const Value* result = parsed.Find("result");
    if (result == nullptr) { throw std::runtime_error("no result"); }
    return *result;
}

void SessionStateKeeper::Update(const f64 nowSeconds)
{
    if (!started_)
    {
        started_ = true;
        startedAt_ = nowSeconds;
        nextAttempt_ = nowSeconds + 1.0;
        nextSave_ = nowSeconds + kSaveIntervalSeconds;
        try
        {
            if (std::filesystem::exists(file_))
            {
                std::ifstream in(file_, std::ios::binary);
                const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                saved_ = rpc::ParseValue(text);
                restoring_ = saved_->IsObject() && saved_->Find("pose") != nullptr;
                lastWritten_ = text;
            }
        }
        catch (const std::exception& exception)
        {
            log::Warning(std::string("The saved session could not be read: ") + exception.what());
        }
    }
    if (restoring_)
    {
        if (nowSeconds < nextAttempt_) { return; }
        nextAttempt_ = nowSeconds + kRestoreIntervalSeconds;
        if (nowSeconds - startedAt_ > kRestoreGiveUpSeconds)
        {
            restoring_ = false;
            return;
        }
        TryRestore();
        return;
    }
    if (nowSeconds >= nextSave_)
    {
        nextSave_ = nowSeconds + kSaveIntervalSeconds;
        TrySave();
    }
}

void SessionStateKeeper::TryRestore()
{
    try
    {
        if (!focused_)
        {
            const Value current = Call("viewport.pose_get", Value::Object{{"id", std::string(kPrimaryView)}});
            const Value* hasCamera = current.Find("has_camera");
            if (hasCamera != nullptr && hasCamera->IsBool() && hasCamera->AsBool())
            {
                restoring_ = false;   // the view already has a camera (the project or the user set one): leave it
                return;
            }
        }
        Value pose = *saved_->Find("pose");
        if (pose.IsObject()) { pose.AsObject().insert_or_assign("id", std::string(kPrimaryView)); }
        const Value target = pose.IsObject() && pose.Find("target_object") != nullptr ? *pose.Find("target_object") : Value();
        const Value result = Call("viewport.pose_set", std::move(pose));
        const Value* restored = result.Find("restored");
        if (restored == nullptr || !restored->IsBool() || !restored->AsBool())
        {
            // The view does not target the saved body (a new session starts on another one): select it and focus it, then the pose applies.
            if (!focused_ && target.IsString())
            {
                static_cast<void>(Call("selection.set", Value::Object{{"ids", Value::Array{target}}}));
                static_cast<void>(Call("viewport.focus_body", Value::Object{}));
                focused_ = true;
            }
            return;
        }
        if (const Value* time = saved_->Find("time"); time != nullptr && time->IsObject())
        {
            // Setting the time pauses the clock, so whether it was playing is set after it.
            Value::Object fields = ClockFields(*time);
            const Value playing = fields.count("playing") != 0U ? fields.at("playing") : Value();
            fields.erase("playing");
            static_cast<void>(Call("time.set", std::move(fields)));
            log::Info(std::string("Session clock: playing was ") + (playing.IsBool() ? (playing.AsBool() ? "true" : "false") : "not saved"));
            if (playing.IsBool()) { static_cast<void>(Call("time.set", Value::Object{{"playing", playing.AsBool()}})); }
        }
        restoring_ = false;
        restored_ = true;
        log::Info("Session restored: the camera and the simulation clock are back where they were.");
    }
    catch (const std::exception& exception)
    {
        // The world is not loaded yet, or the view has no body: the next attempt (the reason is said once).
        if (!restoreFailureLogged_)
        {
            restoreFailureLogged_ = true;
            log::Info(std::string("Restoring the session: waiting (") + exception.what() + ")");
        }
    }
}

void SessionStateKeeper::TrySave()
{
    try
    {
        const Value pose = Call("viewport.pose_get", Value::Object{{"id", std::string(kPrimaryView)}});
        const Value* hasCamera = pose.Find("has_camera");
        if (hasCamera == nullptr || !hasCamera->IsBool() || !hasCamera->AsBool()) { return; }
        const Value time = Call("time.get", Value::Object{});
        const std::string text = rpc::Serialize(Value::Object{{"pose", pose}, {"time", Value(ClockFields(time))}});
        if (text == lastWritten_) { return; }
        std::filesystem::create_directories(file_.parent_path());
        std::ofstream out(file_, std::ios::binary | std::ios::trunc);
        out << text;
        lastWritten_ = text;
    }
    catch (const std::exception& exception)
    {
        // Nothing to save yet (the view has no camera, a method is not registered): said once, the keeper tries again.
        if (!saveFailureLogged_)
        {
            saveFailureLogged_ = true;
            log::Warning(std::string("The session could not be saved yet: ") + exception.what());
        }
    }
}
} // namespace orbit::studio_ui

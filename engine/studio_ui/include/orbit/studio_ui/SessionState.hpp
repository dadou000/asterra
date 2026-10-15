#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rpc/JsonRpc.hpp>

#include <filesystem>
#include <optional>
#include <string>

namespace orbit::studio_ui
{
// Keeps the primary viewport camera pose and the simulation clock (time, rate, playing) in the project, so that a restart of Studio, or the
// automatic handoff to a new generation of it (which starts from nothing), puts the user back where they were: with the camera on the body
// and the clock at the same time, which a placed storm needs (it is built on the first frame that draws clouds).
//
// It works through the RPC methods viewport.pose_get / viewport.pose_set and time.get / time.set, so what is saved is what those expose.
// Call Update every frame from the main loop with a monotonic clock in seconds: at startup it tries to restore every half second until the
// viewport accepts the pose (the world is loaded and the view has its body), for at most 90 s, and gives up at once if the view already has a
// camera; afterwards it saves every 5 s while the view has a camera.
class SessionStateKeeper
{
public:
    SessionStateKeeper(rpc::Dispatcher& dispatcher, std::filesystem::path file);
    void Update(f64 nowSeconds);
    [[nodiscard]] bool Restored() const noexcept { return restored_; }

private:
    [[nodiscard]] rpc::Value Call(const char* method, rpc::Value params) const;
    void TryRestore();
    void TrySave();

    rpc::Dispatcher* dispatcher_{nullptr};
    std::filesystem::path file_;
    bool started_{false};
    bool restoring_{false};
    bool restored_{false};
    bool saveFailureLogged_{false};
    bool restoreFailureLogged_{false};
    bool focused_{false};   // the keeper selected and focused the saved body itself, so a camera on it is not the user's
    f64 startedAt_{0.0};
    f64 nextAttempt_{0.0};
    f64 nextSave_{0.0};
    std::optional<rpc::Value> saved_;
    std::string lastWritten_;
};
} // namespace orbit::studio_ui

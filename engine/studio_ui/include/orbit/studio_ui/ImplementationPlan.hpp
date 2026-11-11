#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rpc/JsonRpc.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace orbit::studio_ui
{
enum class PlanStatus : u8
{
    Idea,
    Ready,
    InProgress,
    Done
};

struct PlanBubble
{
    u64 id{0};
    std::string title;
    std::string description;
    PlanStatus status{PlanStatus::Idea};
    std::optional<u64> after;
    f64 x{0.5};
    f64 y{0.5};
};

struct PlanBubblePatch
{
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::optional<PlanStatus> status;
    std::optional<std::optional<u64>> after;
    std::optional<f64> x;
    std::optional<f64> y;
};

[[nodiscard]] std::string_view PlanStatusName(PlanStatus status) noexcept;
[[nodiscard]] std::optional<PlanStatus> ParsePlanStatus(std::string_view value) noexcept;

// Project-local implementation plan. Mutations from both UI and RPC share
// this store and persist immediately, like the Reports feature.
class ImplementationPlan
{
public:
    void Open(const std::filesystem::path& path);
    [[nodiscard]] u64 Create(std::string title, std::string description = {});
    void Update(u64 id, const PlanBubblePatch& patch);
    [[nodiscard]] bool Remove(u64 id);
    [[nodiscard]] const PlanBubble* Find(u64 id) const noexcept;
    [[nodiscard]] const std::vector<PlanBubble>& Bubbles() const noexcept;
    [[nodiscard]] u64 Revision() const noexcept;
    [[nodiscard]] const std::string& LastSaveError() const noexcept;
    [[nodiscard]] rpc::Value ToValue() const;
    [[nodiscard]] static ImplementationPlan FromValue(const rpc::Value& value);

private:
    void Changed();
    void Save() const;
    [[nodiscard]] bool WouldCycle(u64 id, std::optional<u64> after) const;

    std::vector<PlanBubble> bubbles_;
    u64 nextId_{1};
    u64 revision_{0};
    std::filesystem::path path_;
    mutable std::string lastSaveError_;
};
} // namespace orbit::studio_ui

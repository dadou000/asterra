#include <orbit/studio_ui/ImplementationPlan.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace orbit::studio_ui
{
namespace
{
using rpc::Value;

[[nodiscard]] const Value* Member(const Value& value, const char* key)
{
    return value.IsObject() ? value.Find(key) : nullptr;
}

[[nodiscard]] std::string String(const Value& value, const char* key)
{
    const Value* item = Member(value, key);
    return item != nullptr && item->IsString()
        ? item->AsString()
        : std::string{};
}

[[nodiscard]] u64 Integer(const Value& value, const char* key, const u64 fallback)
{
    const Value* item = Member(value, key);
    if (item == nullptr || !item->IsInteger() || item->AsInteger() < 0)
    {
        return fallback;
    }
    return static_cast<u64>(item->AsInteger());
}

[[nodiscard]] f64 Number(const Value& value, const char* key, const f64 fallback)
{
    const Value* item = Member(value, key);
    return item != nullptr && item->IsNumber() ? item->AsNumber() : fallback;
}
} // namespace

std::string_view PlanStatusName(const PlanStatus status) noexcept
{
    switch (status)
    {
    case PlanStatus::Idea: return "idea";
    case PlanStatus::Ready: return "ready";
    case PlanStatus::InProgress: return "in_progress";
    case PlanStatus::Done: return "done";
    }
    return "idea";
}

std::optional<PlanStatus> ParsePlanStatus(const std::string_view value) noexcept
{
    if (value == "idea") return PlanStatus::Idea;
    if (value == "ready") return PlanStatus::Ready;
    if (value == "in_progress") return PlanStatus::InProgress;
    if (value == "done") return PlanStatus::Done;
    return std::nullopt;
}

void ImplementationPlan::Open(const std::filesystem::path& path)
{
    std::vector<PlanBubble> bubbles;
    u64 nextId = 1;
    std::error_code ignored;
    if (std::filesystem::exists(path, ignored))
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            throw std::runtime_error("Cannot read plan file " + path.generic_string());
        }
        const std::string text(
            (std::istreambuf_iterator<char>(stream)),
            std::istreambuf_iterator<char>());
        ImplementationPlan loaded = FromValue(rpc::ParseValue(text));
        bubbles = std::move(loaded.bubbles_);
        nextId = loaded.nextId_;
    }
    path_ = path;
    bubbles_ = std::move(bubbles);
    nextId_ = nextId;
    ++revision_;
}

u64 ImplementationPlan::Create(std::string title, std::string description)
{
    if (title.empty()) title = "Untitled idea";
    if (title.size() > 200U) title.resize(200U);
    PlanBubble bubble;
    bubble.id = nextId_++;
    bubble.title = std::move(title);
    bubble.description = std::move(description);
    const std::size_t count = bubbles_.size();
    bubble.x = count == 0U ? 0.5 : 0.18 + 0.16 * static_cast<f64>(count % 5U);
    bubble.y = count == 0U ? 0.5 : 0.24 + 0.15 * static_cast<f64>((count / 5U) % 5U);
    bubbles_.push_back(std::move(bubble));
    Changed();
    return bubbles_.back().id;
}

void ImplementationPlan::Update(const u64 id, const PlanBubblePatch& patch)
{
    auto found = std::ranges::find(bubbles_, id, &PlanBubble::id);
    if (found == bubbles_.end()) throw std::out_of_range("Plan bubble not found.");
    PlanBubble candidate = *found;
    if (patch.title.has_value())
    {
        candidate.title = patch.title->substr(0U, 200U);
        if (candidate.title.empty()) throw std::invalid_argument("Plan title cannot be empty.");
    }
    if (patch.description.has_value()) candidate.description = *patch.description;
    if (patch.status.has_value()) candidate.status = *patch.status;
    if (patch.after.has_value()) candidate.after = *patch.after;
    if (patch.x.has_value()) candidate.x = *patch.x;
    if (patch.y.has_value()) candidate.y = *patch.y;
    if (!std::isfinite(candidate.x) || !std::isfinite(candidate.y) ||
        candidate.x < 0.02 || candidate.x > 0.98 ||
        candidate.y < 0.05 || candidate.y > 0.95)
    {
        throw std::invalid_argument("Plan bubble position must be within the canvas.");
    }
    if (candidate.after == id ||
        (candidate.after.has_value() && Find(*candidate.after) == nullptr) ||
        WouldCycle(id, candidate.after))
    {
        throw std::invalid_argument("Schedule link must point to another bubble without creating a cycle.");
    }
    *found = std::move(candidate);
    Changed();
}

bool ImplementationPlan::Remove(const u64 id)
{
    const auto found = std::ranges::find(bubbles_, id, &PlanBubble::id);
    if (found == bubbles_.end()) return false;
    bubbles_.erase(found);
    for (PlanBubble& bubble : bubbles_)
    {
        if (bubble.after == id) bubble.after.reset();
    }
    Changed();
    return true;
}

const PlanBubble* ImplementationPlan::Find(const u64 id) const noexcept
{
    const auto found = std::ranges::find(bubbles_, id, &PlanBubble::id);
    return found == bubbles_.end() ? nullptr : &*found;
}

const std::vector<PlanBubble>& ImplementationPlan::Bubbles() const noexcept
{
    return bubbles_;
}

u64 ImplementationPlan::Revision() const noexcept { return revision_; }
const std::string& ImplementationPlan::LastSaveError() const noexcept { return lastSaveError_; }

rpc::Value ImplementationPlan::ToValue() const
{
    Value::Array entries;
    entries.reserve(bubbles_.size());
    for (const PlanBubble& bubble : bubbles_)
    {
        Value::Object item{
            {"id", static_cast<i64>(bubble.id)},
            {"title", bubble.title},
            {"description", bubble.description},
            {"status", std::string(PlanStatusName(bubble.status))},
            {"x", bubble.x},
            {"y", bubble.y}};
        item.emplace("after", bubble.after.has_value()
            ? Value(static_cast<i64>(*bubble.after))
            : Value(nullptr));
        entries.emplace_back(std::move(item));
    }
    return Value::Object{{"version", 1}, {"next_id", static_cast<i64>(nextId_)}, {"bubbles", std::move(entries)}};
}

ImplementationPlan ImplementationPlan::FromValue(const rpc::Value& value)
{
    if (!value.IsObject()) throw std::invalid_argument("Plan document must be an object.");
    ImplementationPlan plan;
    const Value* entries = Member(value, "bubbles");
    if (entries != nullptr && entries->IsArray())
    {
        for (const Value& item : entries->AsArray())
        {
            if (!item.IsObject()) continue;
            PlanBubble bubble;
            bubble.id = Integer(item, "id", 0);
            bubble.title = String(item, "title");
            bubble.description = String(item, "description");
            const auto status = ParsePlanStatus(String(item, "status"));
            if (!status.has_value() || bubble.id == 0U || bubble.title.empty())
                throw std::invalid_argument("Plan document contains an invalid bubble.");
            bubble.status = *status;
            bubble.x = Number(item, "x", 0.5);
            bubble.y = Number(item, "y", 0.5);
            if (const Value* after = Member(item, "after"); after != nullptr && after->IsInteger() && after->AsInteger() > 0)
                bubble.after = static_cast<u64>(after->AsInteger());
            if (!std::isfinite(bubble.x) || !std::isfinite(bubble.y) || bubble.x < 0.02 || bubble.x > 0.98 || bubble.y < 0.05 || bubble.y > 0.95)
                throw std::invalid_argument("Plan document contains an out-of-bounds bubble.");
            if (std::ranges::any_of(plan.bubbles_, [&bubble](const PlanBubble& other) { return other.id == bubble.id; }))
                throw std::invalid_argument("Plan document contains duplicate bubble ids.");
            plan.nextId_ = std::max(plan.nextId_, bubble.id + 1U);
            plan.bubbles_.push_back(std::move(bubble));
        }
    }
    plan.nextId_ = std::max(plan.nextId_, Integer(value, "next_id", 1));
    for (const PlanBubble& bubble : plan.bubbles_)
    {
        if (bubble.after.has_value() && (plan.Find(*bubble.after) == nullptr || plan.WouldCycle(bubble.id, bubble.after)))
            throw std::invalid_argument("Plan document contains an invalid schedule link.");
    }
    return plan;
}

bool ImplementationPlan::WouldCycle(const u64 id, std::optional<u64> after) const
{
    std::size_t remaining = bubbles_.size() + 1U;
    while (after.has_value() && remaining-- > 0U)
    {
        if (*after == id) return true;
        const PlanBubble* parent = Find(*after);
        if (parent == nullptr) return false;
        after = parent->after;
    }
    return after.has_value();
}

void ImplementationPlan::Changed()
{
    ++revision_;
    Save();
}

void ImplementationPlan::Save() const
{
    if (path_.empty()) return;
    try
    {
        if (path_.has_parent_path()) std::filesystem::create_directories(path_.parent_path());
        auto temporary = path_;
        temporary += ".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream) throw std::runtime_error("Cannot write " + temporary.generic_string());
            stream << rpc::Serialize(ToValue());
            stream.flush();
            if (!stream) throw std::runtime_error("Writing plan file failed.");
        }
#if defined(_WIN32)
        if (MoveFileExW(
                temporary.c_str(),
                path_.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0)
        {
            throw std::system_error(
                static_cast<int>(GetLastError()),
                std::system_category(),
                "Cannot replace plan file " + path_.generic_string());
        }
#else
        std::error_code error;
        std::filesystem::rename(temporary, path_, error);
        if (error)
        {
            throw std::filesystem::filesystem_error(
                "Cannot replace plan file", temporary, path_, error);
        }
#endif
        lastSaveError_.clear();
    }
    catch (const std::exception& exception)
    {
        lastSaveError_ = exception.what();
    }
}
} // namespace orbit::studio_ui

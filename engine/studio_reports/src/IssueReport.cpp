#include <orbit/studio_reports/IssueReport.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <format>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace orbit::studio_reports
{
namespace
{
using rpc::Value;

struct ScopeEntry
{
    ReportScope scope;
    std::string_view name;
};

// Stable display and serialisation order.
constexpr std::array<ScopeEntry, 5> kScopes{{
    {ReportScope::Performance, "performance"},
    {ReportScope::VisualQuality, "visual_quality"},
    {ReportScope::Bug, "bug"},
    {ReportScope::Crash, "crash"},
    {ReportScope::Other, "other"},
}};

[[nodiscard]] std::string Lower(std::string_view text)
{
    std::string out(text);
    std::ranges::transform(
        out,
        out.begin(),
        [](const unsigned char c)
        {
            return static_cast<char>(std::tolower(c));
        });
    return out;
}

[[nodiscard]] bool Contains(
    const std::string_view haystack,
    const std::string_view lowerNeedle)
{
    return Lower(haystack).find(lowerNeedle) != std::string::npos;
}

[[nodiscard]] const Value* Member(const Value& object, const char* key)
{
    return object.IsObject() ? object.Find(key) : nullptr;
}

[[nodiscard]] std::string StringMember(
    const Value& object,
    const char* key,
    std::string fallback = {})
{
    const Value* found = Member(object, key);
    return found != nullptr && found->IsString() ? found->AsString()
                                                 : std::move(fallback);
}

[[nodiscard]] std::string Truncate(std::string text, const std::size_t limit)
{
    if (text.size() > limit)
    {
        text.resize(limit);
        text += "...";
    }
    return text;
}

constexpr std::size_t kMaxTitleLength = 200;
} // namespace

std::string_view StatusName(const ReportStatus status) noexcept
{
    switch (status)
    {
    case ReportStatus::Unresolved:
        return "unresolved";
    case ReportStatus::Pending:
        return "pending";
    case ReportStatus::Resolved:
        return "resolved";
    }
    return "unresolved";
}

std::optional<ReportStatus> ParseStatus(const std::string_view name) noexcept
{
    if (name == "unresolved")
    {
        return ReportStatus::Unresolved;
    }
    if (name == "pending")
    {
        return ReportStatus::Pending;
    }
    if (name == "resolved")
    {
        return ReportStatus::Resolved;
    }
    return std::nullopt;
}

std::string_view ScopeName(const ReportScope scope) noexcept
{
    for (const ScopeEntry& entry : kScopes)
    {
        if (entry.scope == scope)
        {
            return entry.name;
        }
    }
    return "other";
}

std::optional<ReportScope> ParseScope(const std::string_view name) noexcept
{
    for (const ScopeEntry& entry : kScopes)
    {
        if (entry.name == name)
        {
            return entry.scope;
        }
    }
    return std::nullopt;
}

std::vector<std::string> ScopeNames(const u32 scopes)
{
    std::vector<std::string> names;
    for (const ScopeEntry& entry : kScopes)
    {
        if ((scopes & ScopeBit(entry.scope)) != 0U)
        {
            names.emplace_back(entry.name);
        }
    }
    return names;
}

std::string NowIsoUtc()
{
    const auto now = std::chrono::floor<std::chrono::seconds>(
        std::chrono::system_clock::now());
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", now);
}

std::string IssueReport::Label() const
{
    return std::format("R-{:04}", id);
}

bool ReportFilter::Matches(const IssueReport& report) const
{
    if (status.has_value() && report.status != *status)
    {
        return false;
    }
    if (scopes != 0U && (report.scopes & scopes) == 0U)
    {
        return false;
    }
    if (transient.has_value() && report.transient != *transient)
    {
        return false;
    }
    if (!text.empty())
    {
        const std::string needle = Lower(text);
        const bool found =
            Contains(report.title, needle) ||
            Contains(report.description, needle) ||
            std::ranges::any_of(
                report.tags,
                [&needle](const std::string& tag)
                {
                    return Contains(tag, needle);
                });
        if (!found)
        {
            return false;
        }
    }
    return true;
}

u64 ReportStore::Create(
    std::string title,
    const u32 scopes,
    const bool transient,
    std::optional<ReportCondition> start)
{
    if ((scopes & ~kAllScopes) != 0U)
    {
        throw std::invalid_argument("Unknown report scope bits.");
    }

    IssueReport report;
    report.id = nextId_;
    report.title = Truncate(std::move(title), kMaxTitleLength);
    report.scopes = scopes;
    report.transient = transient;
    report.start = std::move(start);
    report.createdAt = NowIsoUtc();
    report.updatedAt = report.createdAt;
    if (report.title.empty())
    {
        report.title = "Untitled " + report.Label();
    }

    ++nextId_;
    reports_.push_back(std::move(report));
    Changed(nullptr);
    return reports_.back().id;
}

const IssueReport* ReportStore::Find(const u64 id) const noexcept
{
    const auto found = std::ranges::find(reports_, id, &IssueReport::id);
    return found == reports_.end() ? nullptr : &*found;
}

const std::vector<IssueReport>& ReportStore::Reports() const noexcept
{
    return reports_;
}

std::vector<const IssueReport*> ReportStore::Query(
    const ReportFilter& filter) const
{
    std::vector<const IssueReport*> matches;
    for (const IssueReport& report : reports_)
    {
        if (filter.Matches(report))
        {
            matches.push_back(&report);
        }
    }
    return matches;
}

IssueReport& ReportStore::Require(const u64 id)
{
    const auto found = std::ranges::find(reports_, id, &IssueReport::id);
    if (found == reports_.end())
    {
        throw std::out_of_range(std::format("No report with id {}.", id));
    }
    return *found;
}

void ReportStore::Update(const u64 id, const ReportPatch& patch)
{
    IssueReport& report = Require(id);

    if (patch.scopes.has_value() && (*patch.scopes & ~kAllScopes) != 0U)
    {
        throw std::invalid_argument("Unknown report scope bits.");
    }

    // Validate everything before touching the report.
    if (patch.title.has_value())
    {
        report.title = Truncate(*patch.title, kMaxTitleLength);
    }
    if (patch.description.has_value())
    {
        report.description = *patch.description;
    }
    if (patch.resolutionNote.has_value())
    {
        report.resolutionNote = *patch.resolutionNote;
    }
    if (patch.status.has_value())
    {
        report.status = *patch.status;
    }
    if (patch.scopes.has_value())
    {
        report.scopes = *patch.scopes;
    }
    if (patch.tags.has_value())
    {
        report.tags = *patch.tags;
    }
    if (patch.transient.has_value())
    {
        report.transient = *patch.transient;
        if (!report.transient)
        {
            // A persistent problem has no end.
            report.end.reset();
        }
    }
    Changed(&report);
}

void ReportStore::CaptureStart(const u64 id, ReportCondition condition)
{
    IssueReport& report = Require(id);
    report.start = std::move(condition);
    Changed(&report);
}

void ReportStore::SetScreenshot(
    const u64 id,
    const bool endCondition,
    std::string relativePath)
{
    IssueReport& report = Require(id);
    auto& condition = endCondition ? report.end : report.start;
    if (!condition.has_value())
    {
        throw std::invalid_argument(
            "The report has no such condition to attach a screenshot to.");
    }
    condition->screenshot = std::move(relativePath);
    Changed(&report);
}

std::filesystem::path ReportStore::AssetPath(const std::string& relativePath) const
{
    if (path_.empty() || relativePath.empty())
    {
        return {};
    }
    return path_.parent_path() / relativePath;
}

void ReportStore::CaptureEnd(const u64 id, ReportCondition condition)
{
    IssueReport& report = Require(id);
    if (!report.transient)
    {
        throw std::invalid_argument(
            "Only a transient report has an ending condition.");
    }
    report.end = std::move(condition);
    Changed(&report);
}

bool ReportStore::Remove(const u64 id)
{
    const auto found = std::ranges::find(reports_, id, &IssueReport::id);
    if (found == reports_.end())
    {
        return false;
    }
    // The report's screenshots go with it (best effort).
    for (const auto* condition : {&found->start, &found->end})
    {
        if (condition->has_value() && !(*condition)->screenshot.empty())
        {
            std::error_code ignored;
            std::filesystem::remove(AssetPath((*condition)->screenshot), ignored);
        }
    }
    reports_.erase(found);
    Changed(nullptr);
    return true;
}

u64 ReportStore::Revision() const noexcept
{
    return revision_;
}

void ReportStore::Changed(IssueReport* const touched)
{
    if (touched != nullptr)
    {
        touched->updatedAt = NowIsoUtc();
    }
    ++revision_;
    if (!path_.empty())
    {
        Save();
    }
}

Value ConditionToValue(const ReportCondition& condition)
{
    Value::Object object{
        {"captured_at", condition.capturedAt},
        {"state", condition.state}};
    if (!condition.screenshot.empty())
    {
        object.emplace("screenshot", condition.screenshot);
    }
    return Value(std::move(object));
}

ReportCondition ConditionFromValue(const Value& value)
{
    ReportCondition condition;
    condition.capturedAt = StringMember(value, "captured_at");
    condition.screenshot = StringMember(value, "screenshot");
    if (const Value* state = Member(value, "state"))
    {
        condition.state = *state;
    }
    return condition;
}

Value ReportToValue(const IssueReport& report)
{
    Value::Array scopes;
    for (std::string& name : ScopeNames(report.scopes))
    {
        scopes.emplace_back(std::move(name));
    }
    Value::Array tags;
    for (const std::string& tag : report.tags)
    {
        tags.emplace_back(tag);
    }

    Value::Object object{
        {"id", static_cast<i64>(report.id)},
        {"label", report.Label()},
        {"title", report.title},
        {"description", report.description},
        {"resolution_note", report.resolutionNote},
        {"status", std::string(StatusName(report.status))},
        {"scopes", Value(std::move(scopes))},
        {"tags", Value(std::move(tags))},
        {"transient", report.transient},
        {"created_at", report.createdAt},
        {"updated_at", report.updatedAt}};
    if (report.start.has_value())
    {
        object.emplace("start", ConditionToValue(*report.start));
    }
    if (report.end.has_value())
    {
        object.emplace("end", ConditionToValue(*report.end));
    }
    return Value(std::move(object));
}

IssueReport ReportFromValue(const Value& value)
{
    if (!value.IsObject())
    {
        throw std::invalid_argument("A report must be a JSON object.");
    }
    const Value* id = value.Find("id");
    if (id == nullptr || !id->IsInteger() || id->AsInteger() <= 0)
    {
        throw std::invalid_argument("A report needs a positive integer id.");
    }

    IssueReport report;
    report.id = static_cast<u64>(id->AsInteger());
    report.title = StringMember(value, "title");
    report.description = StringMember(value, "description");
    report.resolutionNote = StringMember(value, "resolution_note");
    report.createdAt = StringMember(value, "created_at");
    report.updatedAt = StringMember(value, "updated_at");
    if (const auto status = ParseStatus(StringMember(value, "status")))
    {
        report.status = *status;
    }
    if (const Value* scopes = value.Find("scopes");
        scopes != nullptr && scopes->IsArray())
    {
        for (const Value& scope : scopes->AsArray())
        {
            if (scope.IsString())
            {
                if (const auto parsed = ParseScope(scope.AsString()))
                {
                    report.scopes |= ScopeBit(*parsed);
                }
            }
        }
    }
    if (const Value* tags = value.Find("tags");
        tags != nullptr && tags->IsArray())
    {
        for (const Value& tag : tags->AsArray())
        {
            if (tag.IsString())
            {
                report.tags.push_back(tag.AsString());
            }
        }
    }
    if (const Value* transient = value.Find("transient");
        transient != nullptr && transient->IsBool())
    {
        report.transient = transient->AsBool();
    }
    if (const Value* start = value.Find("start");
        start != nullptr && start->IsObject())
    {
        report.start = ConditionFromValue(*start);
    }
    if (const Value* end = value.Find("end");
        end != nullptr && end->IsObject() && report.transient)
    {
        report.end = ConditionFromValue(*end);
    }
    return report;
}

Value ReportStore::ToValue() const
{
    Value::Array reports;
    for (const IssueReport& report : reports_)
    {
        reports.push_back(ReportToValue(report));
    }
    return Value(Value::Object{
        {"version", static_cast<i64>(1)},
        {"next_id", static_cast<i64>(nextId_)},
        {"reports", Value(std::move(reports))}});
}

ReportStore ReportStore::FromValue(const Value& value)
{
    if (!value.IsObject())
    {
        throw std::invalid_argument("A report file must be a JSON object.");
    }

    ReportStore store;
    if (const Value* reports = value.Find("reports");
        reports != nullptr && reports->IsArray())
    {
        for (const Value& entry : reports->AsArray())
        {
            IssueReport report = ReportFromValue(entry);
            if (store.Find(report.id) != nullptr)
            {
                throw std::invalid_argument(
                    std::format("Duplicate report id {}.", report.id));
            }
            store.nextId_ = std::max(store.nextId_, report.id + 1U);
            store.reports_.push_back(std::move(report));
        }
    }
    if (const Value* next = value.Find("next_id");
        next != nullptr && next->IsInteger() && next->AsInteger() > 0)
    {
        store.nextId_ =
            std::max(store.nextId_, static_cast<u64>(next->AsInteger()));
    }
    return store;
}

void ReportStore::Open(const std::filesystem::path& path)
{
    std::error_code ignored;
    if (std::filesystem::exists(path, ignored))
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            throw std::runtime_error(
                "Cannot read report file " + path.generic_string());
        }
        const std::string text(
            (std::istreambuf_iterator<char>(stream)),
            std::istreambuf_iterator<char>());
        ReportStore loaded = FromValue(rpc::ParseValue(text));
        reports_ = std::move(loaded.reports_);
        nextId_ = loaded.nextId_;
    }
    else
    {
        reports_.clear();
        nextId_ = 1;
    }
    path_ = path;
    ++revision_;
}

void ReportStore::Save() const
{
    if (path_.empty())
    {
        return;
    }

    try
    {
        if (path_.has_parent_path())
        {
            std::filesystem::create_directories(path_.parent_path());
        }
        // Write beside the target and swap, so a crash mid-write never
        // leaves a truncated report file.
        std::filesystem::path temporary = path_;
        temporary += ".tmp";
        {
            std::ofstream stream(
                temporary, std::ios::binary | std::ios::trunc);
            if (!stream)
            {
                throw std::runtime_error(
                    "Cannot write " + temporary.generic_string());
            }
            stream << rpc::Serialize(ToValue());
            stream.flush();
            if (!stream)
            {
                throw std::runtime_error(
                    "Writing " + temporary.generic_string() + " failed.");
            }
        }
        std::filesystem::rename(temporary, path_);
        lastSaveError_.clear();
    }
    catch (const std::exception& exception)
    {
        lastSaveError_ = exception.what();
    }
}

const std::filesystem::path& ReportStore::Path() const noexcept
{
    return path_;
}

const std::string& ReportStore::LastSaveError() const noexcept
{
    return lastSaveError_;
}

namespace
{
void AppendCondition(
    std::string& out,
    const char* heading,
    const ReportCondition& condition)
{
    out += std::format("\n## {} ({})\n\n", heading, condition.capturedAt);
    if (!condition.screenshot.empty())
    {
        out += std::format("![{}]({})\n\n", heading, condition.screenshot);
    }
    if (!condition.state.IsObject())
    {
        return;
    }

    if (const Value* view = condition.state.Find("view_text");
        view != nullptr && view->IsObject())
    {
        const std::string text = StringMember(*view, "text");
        if (!text.empty())
        {
            out += "```text\n" + text + "\n```\n\n";
        }
    }
    for (const auto& [key, value] : condition.state.AsObject())
    {
        if (key == "view_text" || key == "performance")
        {
            continue;
        }
        out += std::format(
            "- **{}**: `{}`\n",
            key,
            Truncate(rpc::Serialize(value), 600));
    }
}
} // namespace

std::string ReportToMarkdown(const IssueReport& report)
{
    std::string out = std::format(
        "# {} {}\n\n- Status: {}\n- Scope: ",
        report.Label(),
        report.title,
        StatusName(report.status));

    const std::vector<std::string> scopes = ScopeNames(report.scopes);
    for (std::size_t index = 0; index < scopes.size(); ++index)
    {
        out += (index == 0 ? "" : ", ") + scopes[index];
    }
    if (scopes.empty())
    {
        out += "(none)";
    }
    out += std::format(
        "\n- Kind: {}\n- Created: {}\n",
        report.transient ? "transient (starts and ends)" : "persistent",
        report.createdAt);
    if (!report.tags.empty())
    {
        out += "- Tags: ";
        for (std::size_t index = 0; index < report.tags.size(); ++index)
        {
            out += (index == 0 ? "" : ", ") + report.tags[index];
        }
        out += "\n";
    }
    if (!report.description.empty())
    {
        out += "\n" + report.description + "\n";
    }
    if (report.start.has_value())
    {
        AppendCondition(
            out,
            report.transient ? "Starting condition" : "Condition",
            *report.start);
    }
    if (report.end.has_value())
    {
        AppendCondition(out, "Ending condition", *report.end);
    }
    if (!report.resolutionNote.empty())
    {
        out += "\n## Resolution\n\n" + report.resolutionNote + "\n";
    }
    return out;
}
} // namespace orbit::studio_reports

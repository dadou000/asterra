#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rpc/JsonRpc.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::studio_reports
{
enum class ReportStatus : u8
{
    Unresolved,
    Pending,
    Resolved
};

// What kind of problem it is. A report can carry several, so a stutter that
// only shows with a visual artefact is both Performance and VisualQuality.
enum class ReportScope : u32
{
    Performance = 1U << 0U,
    VisualQuality = 1U << 1U,
    Bug = 1U << 2U,
    Crash = 1U << 3U,
    Other = 1U << 4U
};

inline constexpr u32 kAllScopes = (1U << 5U) - 1U;

[[nodiscard]] constexpr u32 ScopeBit(const ReportScope scope) noexcept
{
    return static_cast<u32>(scope);
}

[[nodiscard]] std::string_view StatusName(ReportStatus status) noexcept;
[[nodiscard]] std::optional<ReportStatus> ParseStatus(
    std::string_view name) noexcept;

[[nodiscard]] std::string_view ScopeName(ReportScope scope) noexcept;
[[nodiscard]] std::optional<ReportScope> ParseScope(
    std::string_view name) noexcept;
// Names of the scopes in a bit set, in a stable order.
[[nodiscard]] std::vector<std::string> ScopeNames(u32 scopes);

// "2026-10-04T12:30:00Z".
[[nodiscard]] std::string NowIsoUtc();

// Everything needed to recreate a moment in Studio: a JSON snapshot of the
// simulation clock, camera pose, view diagnostics and so on. The store treats
// the snapshot as opaque; the Studio capture decides what goes in it.
struct ReportCondition
{
    std::string capturedAt;
    rpc::Value state;
    // Screenshot of the viewport at the moment of capture, relative to the folder
    // the reports file lives in ("Screenshots/R-0004-start.png"); empty when none
    // was taken. Lets a problem be looked at without launching a test.
    std::string screenshot;
};

struct IssueReport
{
    u64 id{0};
    std::string title;
    std::string description;
    std::string resolutionNote;
    ReportStatus status{ReportStatus::Unresolved};
    u32 scopes{0};
    // Free-form labels on top of the scopes.
    std::vector<std::string> tags;
    // A transient problem comes and goes: it has a starting condition and,
    // once it stops, an ending condition. A persistent one has only a start.
    bool transient{false};
    std::optional<ReportCondition> start;
    std::optional<ReportCondition> end;
    std::string createdAt;
    std::string updatedAt;

    // "R-0007".
    [[nodiscard]] std::string Label() const;
};

// Partial update; unset fields are left alone.
struct ReportPatch
{
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::optional<std::string> resolutionNote;
    std::optional<ReportStatus> status;
    std::optional<u32> scopes;
    std::optional<std::vector<std::string>> tags;
    std::optional<bool> transient;
};

struct ReportFilter
{
    std::optional<ReportStatus> status;
    // Matches reports having any of these scopes. 0 matches every report.
    u32 scopes{0};
    std::optional<bool> transient;
    // Case-insensitive substring of the title, description or a tag.
    std::string text;

    [[nodiscard]] bool Matches(const IssueReport& report) const;
};

// One Studio session's reports. Mutations throw std::invalid_argument /
// std::out_of_range on bad input and leave the store unchanged. With a
// persistence path set, every mutation is written to disk straight away so a
// crash never loses a report.
class ReportStore
{
public:
    [[nodiscard]] u64 Create(
        std::string title,
        u32 scopes,
        bool transient,
        std::optional<ReportCondition> start = std::nullopt);

    [[nodiscard]] const IssueReport* Find(u64 id) const noexcept;
    [[nodiscard]] const std::vector<IssueReport>& Reports() const noexcept;
    [[nodiscard]] std::vector<const IssueReport*> Query(
        const ReportFilter& filter) const;

    void Update(u64 id, const ReportPatch& patch);
    void CaptureStart(u64 id, ReportCondition condition);
    // Records (or, with an empty path, clears) the screenshot of a condition.
    void SetScreenshot(u64 id, bool endCondition, std::string relativePath);
    // Where a condition's screenshot lives on disk ("" when the store has no file).
    [[nodiscard]] std::filesystem::path AssetPath(
        const std::string& relativePath) const;
    // Only a transient report has an ending condition.
    void CaptureEnd(u64 id, ReportCondition condition);
    [[nodiscard]] bool Remove(u64 id);

    // Bumped by every change; lets a UI cache derived data.
    [[nodiscard]] u64 Revision() const noexcept;

    [[nodiscard]] rpc::Value ToValue() const;
    [[nodiscard]] static ReportStore FromValue(const rpc::Value& value);

    // Reads path, replacing the contents, and from then on saves every change
    // there. A missing file is an empty store; a damaged one throws.
    void Open(const std::filesystem::path& path);
    void Save() const;
    [[nodiscard]] const std::filesystem::path& Path() const noexcept;
    // The last error from an automatic save ("" when it worked).
    [[nodiscard]] const std::string& LastSaveError() const noexcept;

private:
    [[nodiscard]] IssueReport& Require(u64 id);
    void Changed(IssueReport* touched);

    std::vector<IssueReport> reports_;
    u64 nextId_{1};
    u64 revision_{0};
    std::filesystem::path path_;
    mutable std::string lastSaveError_;
};

[[nodiscard]] rpc::Value ConditionToValue(const ReportCondition& condition);
[[nodiscard]] ReportCondition ConditionFromValue(const rpc::Value& value);
[[nodiscard]] rpc::Value ReportToValue(const IssueReport& report);
[[nodiscard]] IssueReport ReportFromValue(const rpc::Value& value);

// A readable write-up of a report, for pasting into a tracker.
[[nodiscard]] std::string ReportToMarkdown(const IssueReport& report);
} // namespace orbit::studio_reports

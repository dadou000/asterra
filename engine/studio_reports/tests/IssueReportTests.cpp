#include <orbit/studio_reports/IssueReport.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
using namespace orbit::studio_reports;
using orbit::rpc::Value;

int g_failures = 0;

void Expect(const bool condition, const char* what)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << what << "\n";
        ++g_failures;
    }
}

template <typename Exception, typename Fn>
bool Throws(Fn&& fn)
{
    try
    {
        fn();
    }
    catch (const Exception&)
    {
        return true;
    }
    catch (...)
    {
        return false;
    }
    return false;
}

ReportCondition Condition(const char* time)
{
    return {
        .capturedAt = "2026-10-04T12:00:00Z",
        .state = Value(Value::Object{
            {"simulation", Value(Value::Object{{"time_text", time}})}})};
}
} // namespace

int main()
{
    ReportStore store;

    const auto perf = store.Create(
        "Stutter flying over the coast",
        ScopeBit(ReportScope::Performance) | ScopeBit(ReportScope::VisualQuality),
        false,
        Condition("T+0d 00:00:01.000"));
    const auto flicker = store.Create(
        "Cloud flicker at dusk", ScopeBit(ReportScope::VisualQuality), true);

    Expect(perf == 1 && flicker == 2, "ids are sequential");
    Expect(store.Find(perf)->status == ReportStatus::Unresolved,
           "new reports start unresolved");
    Expect(store.Find(perf)->Label() == "R-0001", "label");
    Expect(store.Find(perf)->start.has_value(), "start condition stored");

    // A non-transient report has no ending condition.
    Expect(Throws<std::invalid_argument>(
               [&] { store.CaptureEnd(perf, Condition("x")); }),
           "ending a non-transient report is rejected");
    Expect(!store.Find(perf)->end.has_value(), "rejected end left no trace");

    // A transient one does.
    store.CaptureStart(flicker, Condition("T+1d 00:00:00.000"));
    store.CaptureEnd(flicker, Condition("T+1d 00:00:05.000"));
    Expect(store.Find(flicker)->start.has_value() &&
               store.Find(flicker)->end.has_value(),
           "transient report has start and end");

    // Turning transient off drops the end.
    ReportPatch patch;
    patch.transient = false;
    store.Update(flicker, patch);
    Expect(!store.Find(flicker)->end.has_value(),
           "making a report non-transient clears its end");
    patch.transient = true;
    store.Update(flicker, patch);
    Expect(!store.Find(flicker)->end.has_value(), "end does not come back");

    // Status and scopes.
    ReportPatch status;
    status.status = ReportStatus::Pending;
    status.scopes = ScopeBit(ReportScope::Bug) | ScopeBit(ReportScope::Crash);
    status.tags = std::vector<std::string>{"clouds", "weather"};
    store.Update(perf, status);
    Expect(store.Find(perf)->status == ReportStatus::Pending, "status updated");
    Expect(ScopeNames(store.Find(perf)->scopes).size() == 2, "two scopes");
    Expect(Throws<std::invalid_argument>(
               [&] {
                   ReportPatch bad;
                   bad.scopes = 1U << 20U;
                   store.Update(perf, bad);
               }),
           "unknown scope bits are rejected");
    Expect(Throws<std::out_of_range>(
               [&] { store.Update(99, ReportPatch{}); }),
           "unknown id");

    // Filtering.
    ReportFilter filter;
    filter.status = ReportStatus::Pending;
    Expect(store.Query(filter).size() == 1, "filter by status");
    filter = {};
    filter.scopes = ScopeBit(ReportScope::Crash);
    Expect(store.Query(filter).size() == 1, "filter by scope");
    filter = {};
    filter.transient = true;
    Expect(store.Query(filter).size() == 1, "filter by transient");
    filter = {};
    filter.text = "WEATHER";
    Expect(store.Query(filter).size() == 1, "text filter matches tags");

    // Persistence round trip, including ids surviving a removed report.
    const auto path = std::filesystem::temp_directory_path() /
        "orbit_issue_report_tests" / "reports.json";
    std::filesystem::remove_all(path.parent_path());
    store.Open(path);
    Expect(store.Reports().empty(), "opening a missing file starts empty");

    const auto a = store.Create("a", ScopeBit(ReportScope::Bug), false,
                                Condition("A"));
    const auto b = store.Create("b", ScopeBit(ReportScope::Other), true,
                                Condition("B"));
    store.CaptureEnd(b, Condition("B-end"));
    Expect(store.Remove(b), "remove");
    Expect(!store.Remove(b), "remove twice");
    Expect(std::filesystem::exists(path), "autosave wrote the file");
    Expect(store.LastSaveError().empty(), "autosave has no error");

    ReportStore reopened;
    reopened.Open(path);
    Expect(reopened.Reports().size() == 1, "one report after reload");
    Expect(reopened.Find(a) != nullptr &&
               reopened.Find(a)->start.has_value() &&
               reopened.Find(a)->start->state.Find("simulation") != nullptr,
           "condition snapshot survives");
    const auto c = reopened.Create("c", 0U, false);
    Expect(c > b, "ids are never reused after a delete");
    Expect(reopened.Find(c)->title == "c", "create after reload");

    // Screenshots: stored relative to the reports file, survive a reload, are
    // listed in the Markdown, and go with the report.
    {
        const auto shot = path.parent_path() / "Screenshots" / "R-0001-start.png";
        std::filesystem::create_directories(shot.parent_path());
        std::ofstream(shot) << "not really a png";
        reopened.SetScreenshot(a, false, "Screenshots/R-0001-start.png");
        Expect(reopened.AssetPath("Screenshots/R-0001-start.png") == shot,
               "screenshot path is next to the reports file");
        Expect(Throws<std::invalid_argument>(
                   [&] { reopened.SetScreenshot(a, true, "x.png"); }),
               "no end condition to attach a screenshot to");

        ReportStore again;
        again.Open(path);
        Expect(again.Find(a)->start->screenshot == "Screenshots/R-0001-start.png",
               "screenshot survives a reload");
        Expect(ReportToMarkdown(*again.Find(a)).find("![") != std::string::npos,
               "markdown embeds the screenshot");
        Expect(again.Remove(a), "remove a report with a screenshot");
        Expect(!std::filesystem::exists(shot), "its screenshot file is deleted");
    }

    // Markdown mentions the essentials.
    const std::string markdown = ReportToMarkdown(*reopened.Find(a));
    Expect(markdown.find("R-") != std::string::npos &&
               markdown.find("unresolved") != std::string::npos,
           "markdown export");

    // Damaged file is an error, not silent data loss.
    {
        std::filesystem::path damaged = path;
        damaged.replace_filename("damaged.json");
        std::filesystem::create_directories(damaged.parent_path());
        std::ofstream(damaged) << "{ not json";
        ReportStore broken;
        Expect(Throws<std::exception>([&] { broken.Open(damaged); }),
               "damaged file throws");
    }

    std::filesystem::remove_all(path.parent_path());
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

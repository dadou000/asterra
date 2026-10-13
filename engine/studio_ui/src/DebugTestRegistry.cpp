#include <orbit/studio_ui/DebugTestRegistry.hpp>
#include <cmath>
#include <exception>
#include <utility>

namespace orbit::studio_ui
{
bool DebugTestRegistry::Register(DebugTestDescriptor descriptor)
{
    if (descriptor.id.empty() || descriptor.label.empty() || !descriptor.run)
        return false;
    const auto id = descriptor.id;
    return tests_.emplace(id, std::move(descriptor)).second;
}

bool DebugTestRegistry::Unregister(const std::string_view id)
{
    for (auto it = history_.begin(); it != history_.end();)
        if (it->testId == id) it = history_.erase(it);
        else ++it;
    const auto result = results_.find(id);
    if (result != results_.end()) results_.erase(result);
    const auto test = tests_.find(id);
    if (test == tests_.end()) return false;
    tests_.erase(test);
    return true;
}

std::vector<DebugTestDescriptor> DebugTestRegistry::Catalog() const
{
    std::vector<DebugTestDescriptor> catalog;
    catalog.reserve(tests_.size());
    for (const auto& [id, descriptor] : tests_)
        catalog.push_back(descriptor);
    return catalog;
}

DebugTestResult DebugTestRegistry::Run(
    const std::string_view id, studio_session::StudioSession& session)
{
    const auto it = tests_.find(id);
    if (it == tests_.end())
        return {DebugTestOutcome::NotReady, "Unknown debug test"};
    const auto started = std::chrono::steady_clock::now();
    DebugTestResult result;
    try { result = it->second.run(session); }
    catch (const std::exception& error)
    {
        result = {DebugTestOutcome::Fail, error.what()};
    }
    catch (...)
    {
        result = {DebugTestOutcome::Fail, "Unknown validation exception"};
    }
    const auto finished = std::chrono::steady_clock::now();
    results_.insert_or_assign(std::string(id), result);
    history_.push_back({
        .sequence = ++sequence_,
        .testId = std::string(id),
        .result = result,
        .elapsedMilliseconds = std::chrono::duration<f64, std::milli>(
            finished - started).count(),
        .worldGeneration = session.World().Generation()
    });
    if (history_.size() > kMaxHistory) history_.pop_front();
    return result;
}

DebugTestResult DebugTestRegistry::LastResult(std::string_view id) const
{
    const auto it = results_.find(id);
    return it == results_.end() ? DebugTestResult{} : it->second;
}

std::vector<DebugTestRunRecord> DebugTestRegistry::History(
    const std::string_view id) const
{
    std::vector<DebugTestRunRecord> records;
    for (const auto& record : history_)
        if (record.testId == id) records.push_back(record);
    return records;
}

std::vector<DebugTestRunRecord> DebugTestRegistry::RecentRuns() const
{
    return {history_.begin(), history_.end()};
}

std::string_view DebugTestCategoryName(DebugTestCategory category) noexcept
{
    switch (category)
    {
    case DebugTestCategory::Terrain: return "Terrain";
    case DebugTestCategory::Infrastructure: return "Infrastructure";
    case DebugTestCategory::Lighting: return "Lighting";
    case DebugTestCategory::Weather: return "Weather";
    case DebugTestCategory::Physics: return "Physics";
    case DebugTestCategory::Performance: return "Performance";
    case DebugTestCategory::Regression: return "Regression";
    }
    return "Other";
}

void RegisterBuiltInDebugTests(DebugTestRegistry& registry)
{
    static_cast<void>(registry.Register({
        .id = "terrain.physical_page_sanity",
        .label = "Construction terrain / physical-page sanity",
        .category = DebugTestCategory::Terrain,
        .run = [](studio_session::StudioSession& session) -> DebugTestResult
        {
            for (const auto& runtime : session.TerrainRuntime().Catalog())
            {
                if (runtime.viewportId != "studio.primary") continue;
                const auto address = runtime.observerPhysicalPage;
                const auto page = session.TerrainPhysicalPages().Find(address);
                const auto status = session.TerrainPhysicalPages().PageStatus(address);
                if (!page || !page->material || !status)
                    return {DebugTestOutcome::NotReady,
                        "Observer physical page is not ready"};
                if (page->revisionFingerprint == 0U ||
                    page->revisionFingerprint != status->revisionFingerprint)
                    return {DebugTestOutcome::Fail,
                        "Published physical-page revision mismatch"};
                const auto height = page->SampleSurfaceHeight(0.5, 0.5);
                if (!height || !std::isfinite(*height))
                    return {DebugTestOutcome::Fail,
                        "Missing or nonfinite center height"};
                return {DebugTestOutcome::Pass,
                    "Current physical-page revision; center height finite"};
            }
            return {DebugTestOutcome::NotReady, "No primary terrain viewport"};
        }
    }));
}
} // namespace orbit::studio_ui

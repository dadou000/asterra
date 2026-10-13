#pragma once
#include <orbit/studio_session/StudioSession.hpp>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::studio_ui
{
enum class DebugTestCategory : u8
{
    Terrain, Infrastructure, Lighting, Weather, Physics, Performance, Regression
};

enum class DebugTestOutcome : u8 { NotRun, Pass, Fail, NotReady };

struct DebugTestResult
{
    DebugTestOutcome outcome{DebugTestOutcome::NotRun};
    std::string detail{"Not run"};
};

struct DebugTestDescriptor
{
    std::string id;
    std::string label;
    DebugTestCategory category{DebugTestCategory::Terrain};
    std::function<DebugTestResult(studio_session::StudioSession&)> run;
};

// Instance-owned registry; tests execute only from an explicit user action.
// Unique stable identifiers support later plugin / RPC registration.
class DebugTestRegistry
{
public:
    [[nodiscard]] bool Register(DebugTestDescriptor descriptor);
    [[nodiscard]] bool Unregister(std::string_view id);
    [[nodiscard]] std::vector<DebugTestDescriptor> Catalog() const;
    [[nodiscard]] DebugTestResult Run(
        std::string_view id, studio_session::StudioSession& session);
    [[nodiscard]] DebugTestResult LastResult(std::string_view id) const;
private:
    std::map<std::string, DebugTestDescriptor, std::less<>> tests_;
    std::map<std::string, DebugTestResult, std::less<>> results_;
};

[[nodiscard]] std::string_view DebugTestCategoryName(DebugTestCategory category) noexcept;
void RegisterBuiltInDebugTests(DebugTestRegistry& registry);
} // namespace orbit::studio_ui

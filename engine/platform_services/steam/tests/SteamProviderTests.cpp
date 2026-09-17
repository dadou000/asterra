#include <orbit/platform_services/steam/SteamProvider.hpp>

#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
void Check(const bool condition)
{
    if (!condition)
    {
        std::abort();
    }
}

class FakeSteamBridge final :
    public orbit::platform_services::steam::
        ISteamClientBridge
{
public:
    bool Initialize(
        const orbit::u32 appId) override
    {
        initializedAppId = appId;
        return appId != 0;
    }

    std::optional<
        orbit::platform_services::UserIdentity>
        User() const override
    {
        return orbit::platform_services::
            UserIdentity{
                .id = "1",
                .displayName = "Test User"
            };
    }

    bool GetIntegerStat(
        const std::string_view apiName,
        orbit::i64& value) override
    {
        value = integers[
            std::string(apiName)];
        return true;
    }

    bool GetFloatStat(
        const std::string_view apiName,
        orbit::f64& value) override
    {
        value = floats[
            std::string(apiName)];
        return true;
    }

    bool SetIntegerStat(
        const std::string_view apiName,
        const orbit::i64 value) override
    {
        integers[
            std::string(apiName)] = value;
        return true;
    }

    bool SetFloatStat(
        const std::string_view apiName,
        const orbit::f64 value) override
    {
        floats[
            std::string(apiName)] = value;
        return true;
    }

    bool UnlockAchievement(
        const std::string_view apiName) override
    {
        achievements.insert(
            std::string(apiName));
        return true;
    }

    bool AddTimelineEvent(
        const std::string_view title,
        const std::string_view description,
        const std::string_view icon,
        const orbit::u32 priority,
        const orbit::platform_services::
            TimelineClipPriority) override
    {
        timeline.push_back(
            std::string(title) + "|" +
            std::string(description) + "|" +
            std::string(icon) + "|" +
            std::to_string(priority));
        return true;
    }

    bool StoreStats() override
    {
        flushed = true;
        return true;
    }

    orbit::u32 initializedAppId{0};
    std::unordered_map<
        std::string,
        orbit::i64> integers;
    std::unordered_map<
        std::string,
        orbit::f64> floats;
    std::unordered_set<std::string>
        achievements;
    std::vector<std::string> timeline;
    bool flushed{false};
};
} // namespace

int main()
{
    using namespace orbit::platform_services;
    using namespace orbit::platform_services::steam;

    SteamConfiguration configuration{
        .enabled = true,
        .appId = 480,
        .achievements = {
            {
                .id = "garage.first_repair",
                .steamApiName =
                    "ACH_FIRST_REPAIR"
            }
        },
        .stats = {
            {
                .id = "vehicles.repaired",
                .kind = StatKind::Integer,
                .steamApiName =
                    "STAT_VEHICLES_REPAIRED"
            }
        },
        .timelineEvents = {
            {
                .eventId = "vehicle.repaired",
                .title = "Repair",
                .description = "Repair completed",
                .icon = "steam_achievement",
                .priority = 25,
                .clipPriority =
                    TimelineClipPriority::Standard
            }
        }
    };

    auto bridge =
        std::make_unique<FakeSteamBridge>();
    auto* bridgeView = bridge.get();

    SteamProvider provider(
        configuration,
        std::move(bridge));

    Check(bridgeView->initializedAppId == 480);
    Check(provider.SetIntegerStat(
        "vehicles.repaired",
        3));
    Check(bridgeView->integers[
        "STAT_VEHICLES_REPAIRED"] == 3);

    Check(provider.UnlockAchievement(
        "garage.first_repair"));
    Check(bridgeView->achievements.contains(
        "ACH_FIRST_REPAIR"));

    Check(provider.EmitTimelineEvent({
        .id = "vehicle.repaired"
    }));
    Check(bridgeView->timeline.size() == 1);
    Check(provider.Flush());
    Check(bridgeView->flushed);

    return 0;
}

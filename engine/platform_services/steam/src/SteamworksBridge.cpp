#include <orbit/platform_services/steam/SteamworksBridge.hpp>

#include <steam/steam_api.h>

#include <limits>
#include <stdexcept>
#include <string>

namespace orbit::platform_services::steam
{
namespace
{
[[nodiscard]] ETimelineEventClipPriority
SteamClipPriority(
    const TimelineClipPriority priority) noexcept
{
    switch (priority)
    {
    case TimelineClipPriority::None:
        return
            k_ETimelineEventClipPriority_None;
    case TimelineClipPriority::Standard:
        return
            k_ETimelineEventClipPriority_Standard;
    case TimelineClipPriority::Featured:
        return
            k_ETimelineEventClipPriority_Featured;
    }

    return k_ETimelineEventClipPriority_None;
}

class SteamworksBridge final :
    public ISteamClientBridge
{
public:
    ~SteamworksBridge() override
    {
        if (initialized_)
        {
            SteamAPI_Shutdown();
        }
    }

    [[nodiscard]] bool Initialize(
        const u32 appId) override
    {
        if (initialized_)
        {
            return true;
        }

        if (appId == 0)
        {
            return false;
        }

        initialized_ =
            SteamAPI_Init();

        return initialized_;
    }

    [[nodiscard]] std::optional<UserIdentity>
        User() const override
    {
        if (!initialized_ ||
            SteamUser() == nullptr ||
            SteamFriends() == nullptr)
        {
            return std::nullopt;
        }

        return UserIdentity{
            .id =
                std::to_string(
                    SteamUser()->
                        GetSteamID().
                        ConvertToUint64()),
            .displayName =
                SteamFriends()->
                    GetPersonaName()
        };
    }

    [[nodiscard]] bool GetIntegerStat(
        const std::string_view apiName,
        i64& value) override
    {
        if (!initialized_ ||
            SteamUserStats() == nullptr)
        {
            return false;
        }

        int32 steamValue = 0;

        if (!SteamUserStats()->GetStat(
                std::string(apiName).c_str(),
                &steamValue))
        {
            return false;
        }

        value =
            static_cast<i64>(steamValue);
        return true;
    }

    [[nodiscard]] bool GetFloatStat(
        const std::string_view apiName,
        f64& value) override
    {
        if (!initialized_ ||
            SteamUserStats() == nullptr)
        {
            return false;
        }

        float steamValue = 0.0F;

        if (!SteamUserStats()->GetStat(
                std::string(apiName).c_str(),
                &steamValue))
        {
            return false;
        }

        value =
            static_cast<f64>(steamValue);
        return true;
    }

    [[nodiscard]] bool SetIntegerStat(
        const std::string_view apiName,
        const i64 value) override
    {
        if (!initialized_ ||
            SteamUserStats() == nullptr ||
            value <
                static_cast<i64>(
                    std::numeric_limits<
                        int32>::min()) ||
            value >
                static_cast<i64>(
                    std::numeric_limits<
                        int32>::max()))
        {
            return false;
        }

        return SteamUserStats()->SetStat(
            std::string(apiName).c_str(),
            static_cast<int32>(value));
    }

    [[nodiscard]] bool SetFloatStat(
        const std::string_view apiName,
        const f64 value) override
    {
        return initialized_ &&
            SteamUserStats() != nullptr &&
            SteamUserStats()->SetStat(
                std::string(apiName).c_str(),
                static_cast<float>(value));
    }

    [[nodiscard]] bool UnlockAchievement(
        const std::string_view apiName) override
    {
        return initialized_ &&
            SteamUserStats() != nullptr &&
            SteamUserStats()->
                SetAchievement(
                    std::string(apiName).
                        c_str());
    }

    [[nodiscard]] bool AddTimelineEvent(
        const std::string_view title,
        const std::string_view description,
        const std::string_view icon,
        const u32 priority,
        const TimelineClipPriority
            clipPriority) override
    {
        if (!initialized_ ||
            SteamTimeline() == nullptr)
        {
            return false;
        }

        SteamTimeline()->
            AddInstantaneousTimelineEvent(
                std::string(title).c_str(),
                std::string(description).c_str(),
                std::string(icon).c_str(),
                priority,
                0.0F,
                SteamClipPriority(
                    clipPriority));

        return true;
    }

    [[nodiscard]] bool StoreStats() override
    {
        return initialized_ &&
            SteamUserStats() != nullptr &&
            SteamUserStats()->StoreStats();
    }

private:
    bool initialized_{false};
};
} // namespace

std::unique_ptr<ISteamClientBridge>
CreateSteamworksBridge()
{
    return std::make_unique<
        SteamworksBridge>();
}
} // namespace orbit::platform_services::steam

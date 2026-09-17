#pragma once

#include <orbit/platform_services/PlatformConfig.hpp>
#include <orbit/platform_services/PlatformServices.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace orbit::platform_services::steam
{
class ISteamClientBridge
{
public:
    virtual ~ISteamClientBridge() = default;

    [[nodiscard]] virtual bool Initialize(
        u32 appId) = 0;
    [[nodiscard]] virtual std::optional<UserIdentity>
        User() const = 0;

    [[nodiscard]] virtual bool GetIntegerStat(
        std::string_view apiName,
        i64& value) = 0;
    [[nodiscard]] virtual bool GetFloatStat(
        std::string_view apiName,
        f64& value) = 0;
    [[nodiscard]] virtual bool SetIntegerStat(
        std::string_view apiName,
        i64 value) = 0;
    [[nodiscard]] virtual bool SetFloatStat(
        std::string_view apiName,
        f64 value) = 0;
    [[nodiscard]] virtual bool UnlockAchievement(
        std::string_view apiName) = 0;
    [[nodiscard]] virtual bool AddTimelineEvent(
        std::string_view title,
        std::string_view description,
        std::string_view icon,
        u32 priority,
        TimelineClipPriority clipPriority) = 0;
    [[nodiscard]] virtual bool StoreStats() = 0;
};

class SteamProvider final : public IPlatformProvider
{
public:
    SteamProvider(
        SteamConfiguration configuration,
        std::unique_ptr<ISteamClientBridge> bridge);

    [[nodiscard]] std::string_view Name()
        const noexcept override;
    [[nodiscard]] Capability Capabilities()
        const noexcept override;
    [[nodiscard]] std::optional<UserIdentity> User()
        const override;

    [[nodiscard]] bool GetIntegerStat(
        std::string_view id,
        i64& value) override;
    [[nodiscard]] bool GetFloatStat(
        std::string_view id,
        f64& value) override;
    [[nodiscard]] bool SetIntegerStat(
        std::string_view id,
        i64 value) override;
    [[nodiscard]] bool SetFloatStat(
        std::string_view id,
        f64 value) override;
    [[nodiscard]] bool UnlockAchievement(
        std::string_view id) override;
    [[nodiscard]] bool EmitTimelineEvent(
        const TimelineEvent& event) override;
    [[nodiscard]] bool Flush() override;

private:
    [[nodiscard]] const AchievementMapping*
        FindAchievement(
            std::string_view id) const noexcept;
    [[nodiscard]] const StatMapping* FindStat(
        std::string_view id) const noexcept;
    [[nodiscard]] const TimelineMapping*
        FindTimeline(
            std::string_view eventId) const noexcept;

    SteamConfiguration configuration_;
    std::unique_ptr<ISteamClientBridge> bridge_;
};
} // namespace orbit::platform_services::steam

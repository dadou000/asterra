#pragma once

#include <orbit/platform_services/steam/SteamProvider.hpp>

#include <memory>

namespace orbit::platform_services::steam
{
// Available only when OrbitPlatformServicesSteamworks is built with a
// configured Steamworks SDK. No other Orbit target includes Steamworks
// headers directly.
[[nodiscard]] std::unique_ptr<ISteamClientBridge>
CreateSteamworksBridge();
} // namespace orbit::platform_services::steam

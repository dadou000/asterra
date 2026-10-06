+++
path = "/foundation/platform-services"
title = "Platform services (achievements, stats, timeline, Steam)"
kind = "subsystem"
status = "stable"
summary = "A provider-based boundary for store/platform features: IPlatformProvider with a StandalonProvider fallback, a PlatformServiceRegistry, capabilities, achievement/stat/timeline mappings and a validated SteamConfiguration."
owner_module = "OrbitPlatformServices"
keywords = ["platform services", "steam", "achievements", "stats", "timeline", "provider", "store", "standalone"]
sources = [
  "engine/platform_services/include/orbit/platform_services/PlatformConfig.hpp",
  "engine/platform_services/include/orbit/platform_services/PlatformServices.hpp",
  "engine/platform_services/CMakeLists.txt"]
symbols = ["AchievementMapping", "UserIdentity"]
invariants = [
  "Third-party SDKs (Steam) stay behind the provider adapter; Orbit owns the interface and the SDK is a backend (/rules/architecture)."]
related = []
depends_on = ["/foundation/core"]
used_by = ["/apps/build-service", "/apps/player", "/apps/studio"]
verify = [
  "ctest -R Orbit.PlatformServices"]
verified = "b0a0de7f"
+++



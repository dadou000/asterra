#pragma once

#include <orbit/core/Types.hpp>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace orbit::web_view
{
// The page-side half of the bridge: JavaScript injected into every HTML panel
// before the page's own scripts run. It defines `window.orbit`:
//
//   orbit.available                  true when hosted by Studio
//   orbit.rpc(method, params)        Promise of the JSON-RPC result (rejects with
//                                    an Error carrying .code and .data)
//   orbit.on(event, fn) / off(...)   host-pushed events ({event, data} messages)
//
// A page therefore reaches exactly the RPC surface the MCP tools reach; there
// is no second, page-only API.
[[nodiscard]] std::string_view BootstrapScript() noexcept;

struct BridgeLimits
{
    // Largest page message accepted (a request is small; this guards a
    // misbehaving page, not legitimate use).
    std::size_t maxMessageBytes{8U * 1024U * 1024U};
    // Optional method allow-list; null allows every RPC method.
    std::function<bool(std::string_view method)> allowMethod;
};

using DispatchFunction =
    std::function<std::optional<std::string>(std::string_view request)>;

// Validates one page message (a JSON-RPC 2.0 request string) and forwards it to
// `dispatch`. Returns the response to post back to the page: the dispatcher's
// own response, or a JSON-RPC error when the message is oversized, not valid
// JSON, not a request, or names a method the policy refuses. Notifications
// (no response) return std::nullopt.
[[nodiscard]] std::optional<std::string> HandlePageMessage(
    std::string_view message,
    const DispatchFunction& dispatch,
    const BridgeLimits& limits = {});

// Resolves `relative` (forward or back slashes) under `root` and returns it
// only if the result stays inside `root`: no absolute paths, drive letters,
// or `..` escapes. Used for every file a page may read through the host.
[[nodiscard]] std::optional<std::filesystem::path> ResolveInside(
    const std::filesystem::path& root,
    std::string_view relative);

// Event message for the page's orbit.on(): {"event": name, "data": data}.
// `dataJson` must already be valid JSON.
[[nodiscard]] std::string MakeEventMessage(
    std::string_view eventName,
    std::string_view dataJson);
} // namespace orbit::web_view

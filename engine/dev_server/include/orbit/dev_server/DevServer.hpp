#pragma once

#include <orbit/core/Types.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::dev_server
{
struct DevServerConfig
{
    u16 port{4319};
    std::size_t maxMessageBytes{
        1024U * 1024U};
};

// Loopback-only, non-blocking, line-framed transport for local Orbit
// automation. The transport is protocol-agnostic through MessageHandler;
// legacy whitespace commands remain available while clients migrate to
// JSON-RPC. Each message and response is one UTF-8 line.
//
// Bound to 127.0.0.1 only; never reachable from the network. Polled
// from the main thread once per frame and never blocks, so a
// missing or slow client cannot stall the render loop.
class DevServer
{
public:
    using CommandHandler =
        std::function<std::string(
            const std::vector<std::string>& arguments)>;

    using MessageHandler =
        std::function<std::optional<std::string>(
            std::string_view message)>;

    explicit DevServer(DevServerConfig config = {});
    ~DevServer();

    DevServer(const DevServer&) = delete;
    DevServer& operator=(const DevServer&) = delete;

    // Registers the handler invoked for `name` commands (matched
    // case-insensitively). The handler receives the
    // whitespace-separated arguments following the command name and
    // returns the response line (no trailing newline).
    void RegisterCommand(
        std::string name,
        CommandHandler handler);

    // Installs a transport-level message handler. When present, complete
    // lines are passed through unchanged and legacy whitespace commands are
    // bypassed. Returning nullopt implements notification/no-response
    // protocols such as JSON-RPC 2.0 notifications.
    void SetMessageHandler(
        MessageHandler handler);

    // Accepts a pending connection if there isn't one already, and
    // dispatches any complete lines already received from the
    // current client. Must be called every frame.
    void Poll();

    [[nodiscard]] bool Listening() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::dev_server

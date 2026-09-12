#pragma once

#include <orbit/core/Types.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace orbit::dev_server
{
struct DevServerConfig
{
    u16 port{4319};
};

// A minimal loopback-only TCP text protocol for driving and
// inspecting a running Orbit process from external tooling -- an
// MCP bridge, a Python test harness, etc. One line in, one line
// out: `COMMAND arg0 arg1 ...\n` gets a single-line response back.
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

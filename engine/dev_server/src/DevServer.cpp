#include <orbit/dev_server/DevServer.hpp>

#include <orbit/core/Log.hpp>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <sstream>
#include <unordered_map>

namespace orbit::dev_server
{
namespace
{
constexpr int kReceiveChunkBytes = 4096;

[[nodiscard]] std::string ToUpper(
    std::string text)
{
    std::transform(
        text.begin(),
        text.end(),
        text.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(
                std::toupper(character));
        });

    return text;
}
} // namespace

class DevServer::Impl
{
public:
    explicit Impl(
        const DevServerConfig& config)
        : port_(config.port)
    {
        WSADATA wsaData{};

        if (WSAStartup(
                MAKEWORD(2, 2),
                &wsaData) != 0)
        {
            orbit::log::Warning(
                "Orbit dev server failed to initialize Winsock.");
            return;
        }

        wsaInitialized_ = true;

        listenSocket_ =
            socket(
                AF_INET,
                SOCK_STREAM,
                IPPROTO_TCP);

        if (listenSocket_ ==
            INVALID_SOCKET)
        {
            orbit::log::Warning(
                "Orbit dev server failed to create a socket.");
            return;
        }

        u_long nonBlocking = 1;

        ioctlsocket(
            listenSocket_,
            FIONBIO,
            &nonBlocking);

        int reuseAddress = 1;

        setsockopt(
            listenSocket_,
            SOL_SOCKET,
            SO_REUSEADDR,
            reinterpret_cast<
                const char*>(
                &reuseAddress),
            sizeof(reuseAddress));

        sockaddr_in address{};
        address.sin_family = AF_INET;

        // Loopback only: the dev server is a local testing hook,
        // never a network-facing service.
        address.sin_addr.s_addr =
            htonl(INADDR_LOOPBACK);

        address.sin_port =
            htons(port_);

        if (bind(
                listenSocket_,
                reinterpret_cast<
                    sockaddr*>(
                    &address),
                sizeof(address)) ==
            SOCKET_ERROR)
        {
            orbit::log::Warning(
                std::format(
                    "Orbit dev server failed to bind 127.0.0.1:{}.",
                    port_));

            CloseListenSocket();
            return;
        }

        if (listen(
                listenSocket_,
                1) == SOCKET_ERROR)
        {
            orbit::log::Warning(
                "Orbit dev server failed to listen.");

            CloseListenSocket();
            return;
        }

        orbit::log::Info(
            std::format(
                "Orbit dev server listening on 127.0.0.1:{}.",
                port_));
    }

    ~Impl()
    {
        DisconnectClient();
        CloseListenSocket();

        if (wsaInitialized_)
        {
            WSACleanup();
        }
    }

    void RegisterCommand(
        std::string name,
        DevServer::CommandHandler handler)
    {
        handlers_[ToUpper(
            std::move(name))] =
            std::move(handler);
    }

    void Poll()
    {
        if (listenSocket_ ==
            INVALID_SOCKET)
        {
            return;
        }

        AcceptPendingClient();
        ReceiveAndDispatch();
    }

    [[nodiscard]] bool Listening()
        const noexcept
    {
        return listenSocket_ !=
            INVALID_SOCKET;
    }

private:
    void CloseListenSocket()
    {
        if (listenSocket_ !=
            INVALID_SOCKET)
        {
            closesocket(
                listenSocket_);

            listenSocket_ =
                INVALID_SOCKET;
        }
    }

    void AcceptPendingClient()
    {
        if (clientSocket_ !=
            INVALID_SOCKET)
        {
            // One client at a time -- a test harness holds a single
            // long-lived connection.
            return;
        }

        sockaddr_in clientAddress{};

        int addressLength =
            sizeof(clientAddress);

        const SOCKET accepted =
            accept(
                listenSocket_,
                reinterpret_cast<
                    sockaddr*>(
                    &clientAddress),
                &addressLength);

        if (accepted ==
            INVALID_SOCKET)
        {
            return;
        }

        u_long nonBlocking = 1;

        ioctlsocket(
            accepted,
            FIONBIO,
            &nonBlocking);

        clientSocket_ = accepted;
        receiveBuffer_.clear();

        orbit::log::Info(
            "Orbit dev server client connected.");
    }

    void ReceiveAndDispatch()
    {
        if (clientSocket_ ==
            INVALID_SOCKET)
        {
            return;
        }

        char chunk[kReceiveChunkBytes];

        const int received =
            recv(
                clientSocket_,
                chunk,
                sizeof(chunk),
                0);

        if (received > 0)
        {
            receiveBuffer_.append(
                chunk,
                static_cast<
                    std::size_t>(
                    received));

            DispatchCompleteLines();
            return;
        }

        if (received == 0)
        {
            DisconnectClient();
            return;
        }

        if (WSAGetLastError() !=
            WSAEWOULDBLOCK)
        {
            DisconnectClient();
        }
    }

    void DispatchCompleteLines()
    {
        std::size_t newlinePos = 0;

        while ((newlinePos =
                    receiveBuffer_.find(
                        '\n')) !=
               std::string::npos)
        {
            std::string line =
                receiveBuffer_.substr(
                    0,
                    newlinePos);

            receiveBuffer_.erase(
                0,
                newlinePos + 1);

            if (!line.empty() &&
                line.back() == '\r')
            {
                line.pop_back();
            }

            if (line.empty())
            {
                continue;
            }

            SendLine(
                Dispatch(line));
        }
    }

    [[nodiscard]] std::string Dispatch(
        const std::string& line)
    {
        std::istringstream stream(
            line);

        std::string commandName;
        stream >> commandName;

        std::vector<std::string>
            arguments;

        std::string argument;

        while (stream >> argument)
        {
            arguments.push_back(
                std::move(argument));
        }

        const auto iterator =
            handlers_.find(
                ToUpper(commandName));

        if (iterator ==
            handlers_.end())
        {
            return "ERR unknown command";
        }

        try
        {
            return iterator->second(
                arguments);
        }
        catch (const std::exception&
                   exception)
        {
            return
                std::string("ERR ") +
                exception.what();
        }
    }

    void SendLine(
        const std::string& text)
    {
        if (clientSocket_ ==
            INVALID_SOCKET)
        {
            return;
        }

        std::string payload = text;
        payload.push_back('\n');

        send(
            clientSocket_,
            payload.data(),
            static_cast<int>(
                payload.size()),
            0);
    }

    void DisconnectClient()
    {
        if (clientSocket_ !=
            INVALID_SOCKET)
        {
            closesocket(
                clientSocket_);

            clientSocket_ =
                INVALID_SOCKET;

            orbit::log::Info(
                "Orbit dev server client disconnected.");
        }
    }

    u16 port_;
    bool wsaInitialized_{false};
    SOCKET listenSocket_{INVALID_SOCKET};
    SOCKET clientSocket_{INVALID_SOCKET};
    std::string receiveBuffer_;

    std::unordered_map<
        std::string,
        DevServer::CommandHandler>
        handlers_;
};

DevServer::DevServer(
    const DevServerConfig config)
    : impl_(
        std::make_unique<Impl>(
            config))
{
}

DevServer::~DevServer() = default;

void DevServer::RegisterCommand(
    std::string name,
    CommandHandler handler)
{
    impl_->RegisterCommand(
        std::move(name),
        std::move(handler));
}

void DevServer::Poll()
{
    impl_->Poll();
}

bool DevServer::Listening()
    const noexcept
{
    return impl_->Listening();
}
} // namespace orbit::dev_server

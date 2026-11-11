#include <orbit/rpc/JsonRpc.hpp>
#include <orbit/web_view/WebBridge.hpp>

#include <iostream>
#include <optional>
#include <string>

namespace
{
using namespace orbit;
using namespace orbit::web_view;

int g_failures = 0;

void Check(const bool condition, const char* what)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << what << '\n';
        ++g_failures;
    }
}

[[nodiscard]] i64 ErrorCode(const std::optional<std::string>& response)
{
    if (!response)
        return 0;
    const rpc::Value value = rpc::ParseValue(*response);
    const rpc::Value* error = value.Find("error");
    if (error == nullptr)
        return 0;
    return error->Find("code")->AsInteger();
}
} // namespace

int main()
{
    // The injected script defines the page API and nothing else.
    const std::string_view script = BootstrapScript();
    Check(script.find("window.orbit") != std::string_view::npos, "bootstrap defines window.orbit");
    Check(script.find("postMessage") != std::string_view::npos, "bootstrap posts to the host");
    Check(script.find("rpc(method, params)") != std::string_view::npos, "bootstrap exposes orbit.rpc");

    // A real dispatcher round trip: the page reaches the same RPC surface.
    rpc::Dispatcher dispatcher;
    dispatcher.Register({.name = "test.echo", .description = "echo"},
        [](const rpc::Value& params) { return params; });
    dispatcher.Register({.name = "test.secret", .description = "secret", .mutating = true},
        [](const rpc::Value&) { return rpc::Value(std::string("hidden")); });
    const DispatchFunction dispatch = [&](const std::string_view request)
    { return dispatcher.Dispatch(request); };

    const auto ok = HandlePageMessage(
        R"({"jsonrpc":"2.0","id":7,"method":"test.echo","params":{"a":1}})", dispatch);
    Check(ok.has_value(), "valid request returns a response");
    if (ok)
    {
        const rpc::Value value = rpc::ParseValue(*ok);
        Check(value.Find("id") != nullptr && value.Find("id")->AsInteger() == 7, "response carries the page's id");
        Check(value.Find("result") != nullptr, "response carries the result");
    }

    Check(ErrorCode(HandlePageMessage("not json", dispatch)) == -32700, "invalid JSON is a parse error");
    Check(ErrorCode(HandlePageMessage("[1,2]", dispatch)) == -32600, "a batch/array is refused");
    Check(ErrorCode(HandlePageMessage(R"({"id":1})", dispatch)) == -32600, "a request without a method is refused");
    Check(ErrorCode(HandlePageMessage(R"({"jsonrpc":"2.0","id":1,"method":5})", dispatch)) == -32600,
        "a non-string method is refused");

    BridgeLimits small;
    small.maxMessageBytes = 16U;
    Check(ErrorCode(HandlePageMessage(R"({"jsonrpc":"2.0","id":1,"method":"test.echo"})", dispatch, small)) == -32600,
        "an oversized message is refused");

    // The policy refuses a method but still answers with the page's id so its
    // promise rejects instead of hanging.
    BridgeLimits policy;
    policy.allowMethod = [](const std::string_view method) { return method != "test.secret"; };
    const auto denied = HandlePageMessage(R"({"jsonrpc":"2.0","id":42,"method":"test.secret"})", dispatch, policy);
    Check(ErrorCode(denied) == -32601, "a denied method returns method-not-found");
    if (denied)
        Check(rpc::ParseValue(*denied).Find("id")->AsInteger() == 42, "a denied method echoes the id");
    Check(HandlePageMessage(R"({"jsonrpc":"2.0","id":1,"method":"test.echo"})", dispatch, policy).has_value(),
        "an allowed method still passes");

    Check(ErrorCode(HandlePageMessage(R"({"jsonrpc":"2.0","id":1,"method":"x"})", DispatchFunction{})) == -32603,
        "no dispatcher is an internal error");

    // Notifications produce no response.
    Check(!HandlePageMessage(R"({"jsonrpc":"2.0","method":"test.echo","params":{}})", dispatch).has_value(),
        "a notification has no response");

    // Path containment.
    const std::filesystem::path root = std::filesystem::path("C:/orbit/web");
    Check(ResolveInside(root, "page.html").has_value(), "a plain file resolves");
    Check(ResolveInside(root, "sub/dir/data.json").has_value(), "a nested path resolves");
    Check(ResolveInside(root, "sub\\dir\\data.json").has_value(), "backslashes are accepted");
    Check(ResolveInside(root, "./a/../b.txt").has_value(), "an inner .. that stays inside resolves");
    Check(!ResolveInside(root, "../secret.txt").has_value(), "a leading .. escape is refused");
    Check(!ResolveInside(root, "a/../../secret.txt").has_value(), "a deeper .. escape is refused");
    Check(!ResolveInside(root, "/etc/passwd").has_value(), "an absolute path is refused");
    Check(!ResolveInside(root, "C:/Windows/system.ini").has_value(), "a drive path is refused");
    Check(!ResolveInside(root, "").has_value(), "an empty path is refused");

    // Event messages round trip through the page protocol.
    const std::string event = MakeEventMessage("plan.changed", R"({"revision":3})");
    const rpc::Value parsed = rpc::ParseValue(event);
    Check(parsed.Find("event") != nullptr && parsed.Find("event")->AsString() == "plan.changed", "event name");
    Check(parsed.Find("data") != nullptr && parsed.Find("data")->Find("revision")->AsInteger() == 3, "event data");
    const rpc::Value fallback = rpc::ParseValue(MakeEventMessage("x", "{broken"));
    Check(fallback.Find("data") != nullptr && fallback.Find("data")->IsNull(), "invalid event data becomes null");

    if (g_failures == 0)
        std::cout << "Web bridge: all checks passed.\n";
    return g_failures == 0 ? 0 : 1;
}

#include <orbit/web_view/WebBridge.hpp>

#include <orbit/rpc/JsonRpc.hpp>

#include <algorithm>
#include <exception>
#include <format>

namespace orbit::web_view
{
namespace
{
constexpr std::string_view kBootstrap = R"JS((() => {
  if (window.orbit) return;
  const wv = window.chrome && window.chrome.webview;
  const pending = new Map();
  const listeners = new Map();
  let nextId = 1;
  window.orbit = {
    available: !!wv,
    rpc(method, params) {
      return new Promise((resolve, reject) => {
        if (!wv) { reject(new Error('Studio bridge unavailable')); return; }
        const id = nextId++;
        pending.set(id, { resolve, reject });
        wv.postMessage(JSON.stringify({ jsonrpc: '2.0', id, method, params: params ?? {} }));
      });
    },
    on(event, fn) {
      if (!listeners.has(event)) listeners.set(event, new Set());
      listeners.get(event).add(fn);
    },
    off(event, fn) {
      const set = listeners.get(event);
      if (set) set.delete(fn);
    }
  };
  if (wv) {
    wv.addEventListener('message', (e) => {
      let m = e.data;
      if (typeof m === 'string') { try { m = JSON.parse(m); } catch (_) { return; } }
      if (!m) return;
      if (m.id !== undefined && m.id !== null && pending.has(m.id)) {
        const p = pending.get(m.id);
        pending.delete(m.id);
        if (m.error) {
          const err = new Error(m.error.message || 'RPC error');
          err.code = m.error.code;
          err.data = m.error.data;
          p.reject(err);
        } else {
          p.resolve(m.result);
        }
      } else if (m.event) {
        const set = listeners.get(m.event);
        if (set) set.forEach((fn) => { try { fn(m.data); } catch (_) {} });
      }
    });
  }
})();)JS";

[[nodiscard]] std::string ErrorResponse(
    const i64 code,
    const std::string_view message)
{
    rpc::Value::Object error{
        {"code", code},
        {"message", std::string(message)}};
    rpc::Value::Object response{
        {"jsonrpc", "2.0"},
        {"id", nullptr},
        {"error", std::move(error)}};
    return rpc::Serialize(rpc::Value(std::move(response)));
}
} // namespace

std::string_view BootstrapScript() noexcept
{
    return kBootstrap;
}

std::optional<std::string> HandlePageMessage(
    const std::string_view message,
    const DispatchFunction& dispatch,
    const BridgeLimits& limits)
{
    if (message.size() > limits.maxMessageBytes)
        return ErrorResponse(-32600, "Page message exceeds the bridge size limit.");

    rpc::Value request;
    try
    {
        request = rpc::ParseValue(message);
    }
    catch (const std::exception&)
    {
        return ErrorResponse(-32700, "Page message is not valid JSON.");
    }

    // Batches are not part of the page API: one message, one request.
    if (!request.IsObject())
        return ErrorResponse(-32600, "Page message must be a JSON-RPC request object.");
    const rpc::Value* method = request.Find("method");
    if (method == nullptr || !method->IsString())
        return ErrorResponse(-32600, "Page message has no method.");

    if (limits.allowMethod && !limits.allowMethod(method->AsString()))
    {
        // Echo the id so the page's promise rejects instead of hanging.
        rpc::Value::Object error{
            {"code", i64{-32601}},
            {"message", std::format("Method '{}' is not available to pages.", method->AsString())}};
        rpc::Value::Object response{{"jsonrpc", "2.0"}, {"error", std::move(error)}};
        const rpc::Value* id = request.Find("id");
        response.emplace("id", id != nullptr ? *id : rpc::Value(nullptr));
        return rpc::Serialize(rpc::Value(std::move(response)));
    }

    if (!dispatch)
        return ErrorResponse(-32603, "No dispatcher is attached to the bridge.");
    return dispatch(message);
}

std::optional<std::filesystem::path> ResolveInside(
    const std::filesystem::path& root,
    const std::string_view relative)
{
    if (relative.empty())
        return std::nullopt;
    std::string normalized(relative);
    std::ranges::replace(normalized, '\\', '/');
    // A drive letter or a leading slash is an absolute path in disguise.
    if (normalized.front() == '/' ||
        (normalized.size() >= 2U && normalized[1] == ':'))
        return std::nullopt;

    const std::filesystem::path candidate =
        (root / std::filesystem::path(normalized)).lexically_normal();
    const std::filesystem::path normalRoot = root.lexically_normal();

    // lexically_relative yields a path starting with ".." when candidate is not
    // under root.
    const std::filesystem::path relativeToRoot = candidate.lexically_relative(normalRoot);
    if (relativeToRoot.empty() || relativeToRoot.generic_string().starts_with(".."))
        return std::nullopt;
    for (const auto& part : relativeToRoot)
        if (part == "..")
            return std::nullopt;
    return candidate;
}

std::string MakeEventMessage(
    const std::string_view eventName,
    const std::string_view dataJson)
{
    rpc::Value data;
    try
    {
        data = dataJson.empty() ? rpc::Value(nullptr) : rpc::ParseValue(dataJson);
    }
    catch (const std::exception&)
    {
        data = rpc::Value(nullptr);
    }
    rpc::Value::Object message{
        {"event", std::string(eventName)},
        {"data", std::move(data)}};
    return rpc::Serialize(rpc::Value(std::move(message)));
}
} // namespace orbit::web_view

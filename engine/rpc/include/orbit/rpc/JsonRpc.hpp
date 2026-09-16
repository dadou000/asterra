#pragma once

#include <orbit/core/Types.hpp>

#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace orbit::rpc
{
class Value
{
public:
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value, std::less<>>;
    using Storage =
        std::variant<
            std::monostate,
            bool,
            i64,
            f64,
            std::string,
            Array,
            Object>;

    Value() = default;
    Value(std::nullptr_t) noexcept;
    Value(bool value) noexcept;
    Value(i64 value) noexcept;
    Value(f64 value) noexcept;
    Value(std::string value);
    Value(const char* value);
    Value(Array value);
    Value(Object value);

    [[nodiscard]] bool IsNull() const noexcept;
    [[nodiscard]] bool IsBool() const noexcept;
    [[nodiscard]] bool IsInteger() const noexcept;
    [[nodiscard]] bool IsNumber() const noexcept;
    [[nodiscard]] bool IsString() const noexcept;
    [[nodiscard]] bool IsArray() const noexcept;
    [[nodiscard]] bool IsObject() const noexcept;

    [[nodiscard]] bool AsBool() const;
    [[nodiscard]] i64 AsInteger() const;
    [[nodiscard]] f64 AsNumber() const;
    [[nodiscard]] const std::string& AsString() const;
    [[nodiscard]] const Array& AsArray() const;
    [[nodiscard]] Array& AsArray();
    [[nodiscard]] const Object& AsObject() const;
    [[nodiscard]] Object& AsObject();

    [[nodiscard]] const Value* Find(
        std::string_view key) const noexcept;

    [[nodiscard]] Value* Find(
        std::string_view key) noexcept;

    [[nodiscard]] const Storage& Data() const noexcept;

private:
    Storage data_{};
};

class Error final : public std::runtime_error
{
public:
    Error(
        i64 code,
        std::string message,
        Value data = {});

    [[nodiscard]] i64 Code() const noexcept;
    [[nodiscard]] const Value& Data() const noexcept;

private:
    i64 code_{};
    Value data_{};
};

struct MethodDescriptor
{
    std::string name;
    std::string description;
    bool mutating{false};
};

class Dispatcher
{
public:
    using MethodHandler =
        std::function<Value(const Value& params)>;

    void Register(
        MethodDescriptor descriptor,
        MethodHandler handler);

    [[nodiscard]] bool Unregister(
        std::string_view method);

    [[nodiscard]] std::vector<MethodDescriptor>
    Catalog() const;

    // Dispatches one JSON-RPC 2.0 request or batch. Notifications return
    // std::nullopt. Parse/validation failures are encoded as JSON-RPC errors.
    [[nodiscard]] std::optional<std::string>
    Dispatch(std::string_view payload) const;

private:
    struct Method
    {
        MethodDescriptor descriptor;
        MethodHandler handler;
    };

    std::map<std::string, Method, std::less<>>
        methods_;
};

[[nodiscard]] std::string Serialize(
    const Value& value);

[[nodiscard]] Value ParseValue(
    std::string_view json);
} // namespace orbit::rpc

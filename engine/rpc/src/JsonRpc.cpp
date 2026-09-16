#include <orbit/rpc/JsonRpc.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace orbit::rpc
{
namespace
{
using Json = nlohmann::json;

[[nodiscard]] Value FromJson(
    const Json& json)
{
    if (json.is_null())
    {
        return {};
    }

    if (json.is_boolean())
    {
        return Value(json.get<bool>());
    }

    if (json.is_number_integer() ||
        json.is_number_unsigned())
    {
        if (json.is_number_unsigned())
        {
            const auto value =
                json.get<u64>();

            if (value >
                static_cast<u64>(
                    std::numeric_limits<i64>::max()))
            {
                return Value(
                    static_cast<f64>(value));
            }

            return Value(
                static_cast<i64>(value));
        }

        return Value(
            json.get<i64>());
    }

    if (json.is_number_float())
    {
        return Value(
            json.get<f64>());
    }

    if (json.is_string())
    {
        return Value(
            json.get<std::string>());
    }

    if (json.is_array())
    {
        Value::Array result;
        result.reserve(json.size());

        for (const Json& item : json)
        {
            result.push_back(
                FromJson(item));
        }

        return Value(
            std::move(result));
    }

    if (json.is_object())
    {
        Value::Object result;

        for (auto iterator =
                 json.begin();
             iterator != json.end();
             ++iterator)
        {
            result.emplace(
                iterator.key(),
                FromJson(
                    iterator.value()));
        }

        return Value(
            std::move(result));
    }

    throw std::invalid_argument(
        "Unsupported JSON value.");
}

[[nodiscard]] Json ToJson(
    const Value& value)
{
    const auto& data =
        value.Data();

    if (std::holds_alternative<
            std::monostate>(data))
    {
        return nullptr;
    }

    if (const auto* item =
            std::get_if<bool>(&data))
    {
        return *item;
    }

    if (const auto* item =
            std::get_if<i64>(&data))
    {
        return *item;
    }

    if (const auto* item =
            std::get_if<f64>(&data))
    {
        if (!std::isfinite(*item))
        {
            throw std::invalid_argument(
                "JSON cannot encode non-finite numbers.");
        }

        return *item;
    }

    if (const auto* item =
            std::get_if<std::string>(&data))
    {
        return *item;
    }

    if (const auto* item =
            std::get_if<Value::Array>(&data))
    {
        Json array =
            Json::array();

        for (const Value& child :
             *item)
        {
            array.push_back(
                ToJson(child));
        }

        return array;
    }

    const auto& object =
        std::get<Value::Object>(data);

    Json result =
        Json::object();

    for (const auto& [key, child] :
         object)
    {
        result[key] =
            ToJson(child);
    }

    return result;
}

[[nodiscard]] Json ErrorResponse(
    const Json& id,
    const i64 code,
    const std::string_view message,
    const Value* data = nullptr)
{
    Json error = {
        {"code", code},
        {"message", message}
    };

    if (data != nullptr &&
        !data->IsNull())
    {
        error["data"] =
            ToJson(*data);
    }

    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", std::move(error)}
    };
}

[[nodiscard]] bool ValidId(
    const Json& id) noexcept
{
    return id.is_null() ||
        id.is_string() ||
        id.is_number_integer() ||
        id.is_number_unsigned() ||
        id.is_number_float();
}

[[nodiscard]] std::optional<Json>
DispatchSingle(
    const Json& request,
    const std::map<
        std::string,
        Dispatcher::Method,
        std::less<>>& methods)
{
    if (!request.is_object())
    {
        return ErrorResponse(
            nullptr,
            -32600,
            "Invalid Request");
    }

    const auto version =
        request.find("jsonrpc");
    const auto methodNode =
        request.find("method");

    if (version == request.end() ||
        !version->is_string() ||
        version->get<std::string>() != "2.0" ||
        methodNode == request.end() ||
        !methodNode->is_string())
    {
        return ErrorResponse(
            request.value(
                "id",
                Json(nullptr)),
            -32600,
            "Invalid Request");
    }

    const auto idIterator =
        request.find("id");

    const bool notification =
        idIterator == request.end();

    Json id = nullptr;

    if (!notification)
    {
        if (!ValidId(*idIterator))
        {
            return ErrorResponse(
                nullptr,
                -32600,
                "Invalid Request");
        }

        id = *idIterator;
    }

    Json paramsJson =
        Json::object();

    const auto params =
        request.find("params");

    if (params != request.end())
    {
        if (!params->is_object() &&
            !params->is_array())
        {
            if (notification)
            {
                return std::nullopt;
            }

            return ErrorResponse(
                id,
                -32602,
                "Invalid params");
        }

        paramsJson =
            *params;
    }

    const std::string methodName =
        methodNode->get<std::string>();

    const auto found =
        methods.find(
            methodName);

    if (found == methods.end())
    {
        if (notification)
        {
            return std::nullopt;
        }

        return ErrorResponse(
            id,
            -32601,
            "Method not found");
    }

    try
    {
        const Value result =
            found->second.handler(
                FromJson(paramsJson));

        if (notification)
        {
            return std::nullopt;
        }

        return Json{
            {"jsonrpc", "2.0"},
            {"id", id},
            {"result", ToJson(result)}
        };
    }
    catch (const Error& error)
    {
        if (notification)
        {
            return std::nullopt;
        }

        return ErrorResponse(
            id,
            error.Code(),
            error.what(),
            &error.Data());
    }
    catch (const std::exception& exception)
    {
        if (notification)
        {
            return std::nullopt;
        }

        const Value data(
            exception.what());

        return ErrorResponse(
            id,
            -32603,
            "Internal error",
            &data);
    }
}
} // namespace

Value::Value(std::nullptr_t) noexcept
{
}

Value::Value(
    const bool value) noexcept
    : data_(value)
{
}

Value::Value(
    const i64 value) noexcept
    : data_(value)
{
}

Value::Value(
    const f64 value) noexcept
    : data_(value)
{
}

Value::Value(
    std::string value)
    : data_(std::move(value))
{
}

Value::Value(
    const char* value)
    : data_(
          value != nullptr
              ? std::string(value)
              : std::string{})
{
}

Value::Value(
    Array value)
    : data_(std::move(value))
{
}

Value::Value(
    Object value)
    : data_(std::move(value))
{
}

bool Value::IsNull() const noexcept
{
    return std::holds_alternative<
        std::monostate>(data_);
}

bool Value::IsBool() const noexcept
{
    return std::holds_alternative<bool>(
        data_);
}

bool Value::IsInteger() const noexcept
{
    return std::holds_alternative<i64>(
        data_);
}

bool Value::IsNumber() const noexcept
{
    return IsInteger() ||
        std::holds_alternative<f64>(
            data_);
}

bool Value::IsString() const noexcept
{
    return std::holds_alternative<
        std::string>(data_);
}

bool Value::IsArray() const noexcept
{
    return std::holds_alternative<Array>(
        data_);
}

bool Value::IsObject() const noexcept
{
    return std::holds_alternative<Object>(
        data_);
}

bool Value::AsBool() const
{
    return std::get<bool>(data_);
}

i64 Value::AsInteger() const
{
    return std::get<i64>(data_);
}

f64 Value::AsNumber() const
{
    if (const auto* integer =
            std::get_if<i64>(&data_))
    {
        return static_cast<f64>(
            *integer);
    }

    return std::get<f64>(data_);
}

const std::string&
Value::AsString() const
{
    return std::get<std::string>(
        data_);
}

const Value::Array&
Value::AsArray() const
{
    return std::get<Array>(data_);
}

Value::Array&
Value::AsArray()
{
    return std::get<Array>(data_);
}

const Value::Object&
Value::AsObject() const
{
    return std::get<Object>(data_);
}

Value::Object&
Value::AsObject()
{
    return std::get<Object>(data_);
}

const Value* Value::Find(
    const std::string_view key) const noexcept
{
    const auto* object =
        std::get_if<Object>(&data_);

    if (object == nullptr)
    {
        return nullptr;
    }

    const auto found =
        object->find(key);

    return found == object->end()
        ? nullptr
        : &found->second;
}

Value* Value::Find(
    const std::string_view key) noexcept
{
    auto* object =
        std::get_if<Object>(&data_);

    if (object == nullptr)
    {
        return nullptr;
    }

    const auto found =
        object->find(key);

    return found == object->end()
        ? nullptr
        : &found->second;
}

const Value::Storage&
Value::Data() const noexcept
{
    return data_;
}

Error::Error(
    const i64 code,
    std::string message,
    Value data)
    : std::runtime_error(
          std::move(message)),
      code_(code),
      data_(std::move(data))
{
}

i64 Error::Code() const noexcept
{
    return code_;
}

const Value& Error::Data() const noexcept
{
    return data_;
}

void Dispatcher::Register(
    MethodDescriptor descriptor,
    MethodHandler handler)
{
    if (descriptor.name.empty() ||
        !handler)
    {
        throw std::invalid_argument(
            "RPC method requires name and handler.");
    }

    const std::string key =
        descriptor.name;

    if (methods_.contains(key))
    {
        throw std::invalid_argument(
            "RPC method is already registered: " +
            key);
    }

    methods_.emplace(
        key,
        Method{
            .descriptor =
                std::move(descriptor),
            .handler =
                std::move(handler)
        });
}

bool Dispatcher::Unregister(
    const std::string_view method)
{
    return methods_.erase(
        std::string(method)) != 0;
}

std::vector<MethodDescriptor>
Dispatcher::Catalog() const
{
    std::vector<MethodDescriptor>
        result;
    result.reserve(
        methods_.size());

    for (const auto& [name, method] :
         methods_)
    {
        static_cast<void>(name);
        result.push_back(
            method.descriptor);
    }

    return result;
}

std::optional<std::string>
Dispatcher::Dispatch(
    const std::string_view payload) const
{
    Json document;

    try
    {
        document =
            Json::parse(
                payload.begin(),
                payload.end());
    }
    catch (const Json::parse_error&)
    {
        return ErrorResponse(
                   nullptr,
                   -32700,
                   "Parse error").
            dump();
    }

    if (document.is_array())
    {
        if (document.empty())
        {
            return ErrorResponse(
                       nullptr,
                       -32600,
                       "Invalid Request").
                dump();
        }

        Json responses =
            Json::array();

        for (const Json& request :
             document)
        {
            if (auto response =
                    DispatchSingle(
                        request,
                        methods_);
                response.has_value())
            {
                responses.push_back(
                    std::move(*response));
            }
        }

        if (responses.empty())
        {
            return std::nullopt;
        }

        return responses.dump();
    }

    const auto response =
        DispatchSingle(
            document,
            methods_);

    return response.has_value()
        ? std::optional(
              response->dump())
        : std::nullopt;
}

std::string Serialize(
    const Value& value)
{
    return ToJson(value).dump();
}

Value ParseValue(
    const std::string_view json)
{
    try
    {
        return FromJson(
            Json::parse(
                json.begin(),
                json.end()));
    }
    catch (const Json::parse_error&
               exception)
    {
        throw std::invalid_argument(
            exception.what());
    }
}
} // namespace orbit::rpc

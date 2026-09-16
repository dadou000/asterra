#include <orbit/rpc/JsonRpc.hpp>

#include <cassert>
#include <string>

int main()
{
    orbit::rpc::Dispatcher dispatcher;

    dispatcher.Register(
        {
            .name = "system.ping",
            .description = "Returns a structured pong.",
            .mutating = false
        },
        [](const orbit::rpc::Value& params)
        {
            orbit::rpc::Value::Object result{
                {"pong", true}
            };

            if (const auto* value =
                    params.Find("value");
                value != nullptr)
            {
                result.emplace(
                    "value",
                    *value);
            }

            return orbit::rpc::Value(
                std::move(result));
        });

    dispatcher.Register(
        {
            .name = "test.fail",
            .description = "Produces a typed application error.",
            .mutating = false
        },
        [](const orbit::rpc::Value&)
            -> orbit::rpc::Value
        {
            throw orbit::rpc::Error(
                1001,
                "Rejected",
                orbit::rpc::Value(
                    orbit::rpc::Value::Object{
                        {"reason", "test"}
                    }));
        });

    const auto response =
        dispatcher.Dispatch(
            R"({"jsonrpc":"2.0","id":7,"method":"system.ping","params":{"value":"hello"}})");

    assert(response.has_value());

    const auto root =
        orbit::rpc::ParseValue(*response);

    assert(root.IsObject());
    assert(
        root.Find("jsonrpc")->AsString() ==
        "2.0");
    assert(
        root.Find("id")->AsInteger() == 7);

    const auto* result =
        root.Find("result");

    if (result == nullptr)
    {
        return 1;
    }
    assert(
        result->Find("pong")->AsBool());
    assert(
        result->Find("value")->AsString() ==
        "hello");

    const auto notification =
        dispatcher.Dispatch(
            R"({"jsonrpc":"2.0","method":"system.ping","params":{}})");

    assert(!notification.has_value());

    const auto missing =
        dispatcher.Dispatch(
            R"({"jsonrpc":"2.0","id":"a","method":"missing"})");

    assert(missing.has_value());

    const auto missingValue =
        orbit::rpc::ParseValue(*missing);

    assert(
        missingValue.
            Find("error")->
            Find("code")->
            AsInteger() == -32601);

    const auto malformed =
        dispatcher.Dispatch("{");

    assert(malformed.has_value());
    assert(
        orbit::rpc::ParseValue(*malformed).
            Find("error")->
            Find("code")->
            AsInteger() == -32700);

    const auto applicationError =
        dispatcher.Dispatch(
            R"({"jsonrpc":"2.0","id":2,"method":"test.fail"})");

    assert(applicationError.has_value());

    const auto errorValue =
        orbit::rpc::ParseValue(
            *applicationError);

    assert(
        errorValue.
            Find("error")->
            Find("code")->
            AsInteger() == 1001);
    assert(
        errorValue.
            Find("error")->
            Find("data")->
            Find("reason")->
            AsString() == "test");

    const auto batch =
        dispatcher.Dispatch(
            R"([{"jsonrpc":"2.0","id":1,"method":"system.ping"},{"jsonrpc":"2.0","method":"system.ping"},{"jsonrpc":"2.0","id":2,"method":"missing"}])");

    assert(batch.has_value());

    const auto batchValue =
        orbit::rpc::ParseValue(*batch);

    assert(batchValue.IsArray());
    assert(batchValue.AsArray().size() == 2);

    const auto catalog =
        dispatcher.Catalog();

    assert(catalog.size() == 2);
    assert(catalog[0].name == "system.ping");
    assert(catalog[1].name == "test.fail");

    assert(
        dispatcher.Unregister(
            "test.fail"));

    assert(
        dispatcher.Catalog().size() == 1);

    return 0;
}

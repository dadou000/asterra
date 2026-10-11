#include <orbit/studio_ui/LightningRpc.hpp>

#include <cstdio>
#include <string>

using orbit::rpc::Value;

namespace
{
int failures = 0;
void Check(bool ok, const char* what)
{
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}
Value Call(const orbit::rpc::Dispatcher& d, const std::string& method, const std::string& params = "{}")
{
    const auto response = d.Dispatch(R"({"jsonrpc":"2.0","id":1,"method":")" + method + R"(","params":)" + params + "}");
    return orbit::rpc::ParseValue(*response);
}
} // namespace

int main()
{
    orbit::lighting::TransientFlashRegistry registry;
    orbit::rpc::Dispatcher dispatcher;
    orbit::studio_ui::RegisterLightningRpc(
        dispatcher,
        {[&](const orbit::lighting::FlashRequest& r) { return registry.Fire(r, 0.0); },
         [&] { return registry.Status(0.0); },
         [&] { registry.Clear(); }});

    Value fired = Call(dispatcher, "lightning.fire", R"({"distance_m":2500,"strokes":2,"seed":5})");
    Check(fired.Find("result") != nullptr && fired.Find("result")->Find("id")->AsNumber() >= 1.0, "fire returns an id");
    Check(Call(dispatcher, "lightning.status").Find("result")->Find("count")->AsNumber() == 1.0, "status lists the flash");

    Value absolute = Call(dispatcher, "lightning.fire", R"({"base_m":[1,2,3]})");
    Check(absolute.Find("result") != nullptr, "absolute flash accepted");
    const Value status = Call(dispatcher, "lightning.status");
    const auto& list = status.Find("result")->Find("flashes")->AsArray();
    Check(list.size() == 2 && list[1].Find("placed")->AsBool() && !list[0].Find("placed")->AsBool(), "only the absolute flash is placed");

    Check(Call(dispatcher, "lightning.fire", R"({"strokes":99})").Find("error") != nullptr, "out-of-range strokes rejected");
    Check(Call(dispatcher, "lightning.fire", R"({"base_m":[1,2]})").Find("error") != nullptr, "short base_m rejected");
    Check(Call(dispatcher, "lightning.fire", R"({"energy_j":"a lot"})").Find("error") != nullptr, "non-number rejected");
    Check(registry.Status(0.0).size() == 2, "rejected calls fire nothing");

    Check(Call(dispatcher, "lightning.clear").Find("result") != nullptr && registry.Empty(), "clear empties the registry");
    std::printf(failures == 0 ? "Lightning RPC tests passed\n" : "%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

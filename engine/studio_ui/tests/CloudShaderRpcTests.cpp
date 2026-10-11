#include <orbit/studio_ui/CloudShaderRpc.hpp>

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
    std::string current;
    int compiles = 0;
    orbit::rpc::Dispatcher dispatcher;
    orbit::studio_ui::RegisterCloudShaderRpc(
        dispatcher,
        {[&] { return current; },
         [&](const std::string& defines) -> std::string
         {
             ++compiles;
             if (defines.find("BROKEN") != std::string::npos) { return "syntax error"; }
             current = defines;
             return {};
         }});

    Check(Call(dispatcher, "clouds.get_defines").Find("result")->Find("defines")->AsString().empty(), "starts with the shipped shaders");

    const Value set = Call(dispatcher, "clouds.set_defines", R"({"defines":"CLOUD_MAX_STEPS=160;CLOUD_FAR_EARLYOUT=0"})");
    Check(set.Find("result") != nullptr && set.Find("result")->Find("error")->AsString().empty(), "set succeeds");
    Check(set.Find("result")->Find("defines")->AsString() == "CLOUD_MAX_STEPS=160;CLOUD_FAR_EARLYOUT=0", "set reports the defines in use");
    Check(set.Find("result")->Find("compile_ms")->AsNumber() >= 0.0, "set reports a compile time");
    Check(current == "CLOUD_MAX_STEPS=160;CLOUD_FAR_EARLYOUT=0", "the host received the defines");

    const Value broken = Call(dispatcher, "clouds.set_defines", R"({"defines":"BROKEN=1"})");
    Check(broken.Find("result") != nullptr && broken.Find("result")->Find("error")->AsString() == "syntax error", "a compile error is returned, not thrown");
    Check(current == "CLOUD_MAX_STEPS=160;CLOUD_FAR_EARLYOUT=0", "a compile error keeps the previous shaders");

    Check(Call(dispatcher, "clouds.set_defines", R"({"defines":""})").Find("result") != nullptr && current.empty(), "an empty string restores the shipped shaders");

    const int before = compiles;
    Check(Call(dispatcher, "clouds.set_defines", R"({"defines":5})").Find("error") != nullptr, "a non-string is rejected");
    Check(Call(dispatcher, "clouds.set_defines").Find("error") != nullptr, "missing defines is rejected");
    Check(Call(dispatcher, "clouds.set_defines", "{\"defines\":\"" + std::string(5000, 'A') + "\"}").Find("error") != nullptr, "an oversized string is rejected");
    Check(compiles == before, "rejected calls compile nothing");

    if (failures == 0) { std::printf("CloudShaderRpc: all checks passed\n"); }
    return failures == 0 ? 0 : 1;
}

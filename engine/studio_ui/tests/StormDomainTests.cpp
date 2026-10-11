#include <orbit/studio_ui/StormDomain.hpp>

#include <cstdio>
#include <string>

using orbit::rpc::Value;
using namespace orbit::studio_ui;

namespace
{
int failures = 0;
void Check(const bool ok, const char* what)
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
    StormDomainMailbox mailbox;
    Check(mailbox.Status().state == StormStatus::State::None, "starts with no storm");
    Check(!mailbox.TakePlace().has_value() && !mailbox.TakeClear(), "nothing queued at the start");

    StormPlaceRequest request;
    request.latitudeDegrees = 12.5;
    request.longitudeDegrees = -40.0;
    Check(mailbox.Place(request).empty(), "a valid request is queued");
    Check(mailbox.Status().state == StormStatus::State::Pending, "a queued request is pending");

    StormPlaceRequest bad = request;
    bad.cells = 100;
    Check(!mailbox.Place(bad).empty(), "cells must be a power of two");
    bad = request;
    bad.cells = 512;
    Check(!mailbox.Place(bad).empty(), "cells above 256 are rejected");
    bad = request;
    bad.latitudeDegrees = 95.0;
    Check(!mailbox.Place(bad).empty(), "latitude beyond the pole is rejected");
    bad = request;
    bad.spongeCells = 60;
    Check(!mailbox.Place(bad).empty(), "a sponge wider than a quarter of the domain is rejected");

    // The render thread takes the request once, and its status cannot overwrite a newer request.
    const auto taken = mailbox.TakePlace();
    Check(taken.has_value() && taken->latitudeDegrees == 12.5 && taken->longitudeDegrees == -40.0, "the request arrives intact");
    Check(!mailbox.TakePlace().has_value(), "a request is taken once");
    StormStatus active;
    active.state = StormStatus::State::Active;
    active.stormSeconds = 3600.0;
    mailbox.Publish(active);
    Check(mailbox.Status().state == StormStatus::State::Active && mailbox.Status().stormSeconds == 3600.0, "the render thread publishes status");
    Check(mailbox.Place(request).empty(), "a second placement replaces the first");
    mailbox.Publish(active);
    Check(mailbox.Status().state == StormStatus::State::Pending, "a stale status does not hide a queued request");
    mailbox.Clear();
    Check(mailbox.Status().state == StormStatus::State::None && mailbox.TakeClear() && !mailbox.TakeClear(), "clear is queued once");
    Check(!mailbox.TakePlace().has_value(), "clear drops a queued placement");

    orbit::rpc::Dispatcher dispatcher;
    RegisterStormDomainRpc(dispatcher, mailbox);
    Check(Call(dispatcher, "storm.status").Find("result")->Find("state")->AsString() == "none", "status over RPC");
    const Value placed = Call(dispatcher, "storm.place", R"({"latitude_deg":30.0,"longitude_deg":-97.0,"cells":64,"spin_minutes":20,"ice":false})");
    Check(placed.Find("result") != nullptr && placed.Find("result")->Find("error")->AsString().empty(), "place over RPC");
    const auto queued = mailbox.TakePlace();
    Check(queued.has_value() && queued->cells == 64U && queued->spinMinutes == 20.0F && !queued->ice && queued->latitudeDegrees == 30.0,
        "the RPC parameters reach the request");
    Check(Call(dispatcher, "storm.place", R"({"latitude_deg":30.0})").Find("error") != nullptr, "longitude is required");
    Check(Call(dispatcher, "storm.place", R"({"latitude_deg":30.0,"longitude_deg":1.0,"ice":3})").Find("error") != nullptr, "ice must be a boolean");
    const Value rejected = Call(dispatcher, "storm.place", R"({"latitude_deg":30.0,"longitude_deg":1.0,"cells":100})");
    Check(rejected.Find("result") != nullptr && !rejected.Find("result")->Find("error")->AsString().empty(), "a bad value is reported, not thrown");
    (void)Call(dispatcher, "storm.clear");
    Check(mailbox.TakeClear(), "clear over RPC");

    if (failures == 0) { std::printf("StormDomain: all checks passed\n"); }
    return failures == 0 ? 0 : 1;
}

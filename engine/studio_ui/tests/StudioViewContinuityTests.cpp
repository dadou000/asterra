#include <orbit/studio_ui/StudioViewContinuity.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace
{
using namespace orbit;
using namespace orbit::studio_ui;

void Check(const bool condition, const std::string_view what)
{
    if (!condition)
    {
        std::cerr << "Studio view continuity test failed: " << what << "\n";
        std::exit(EXIT_FAILURE);
    }
}

[[nodiscard]] StudioViewContinuitySnapshot Sample()
{
    StudioViewPose pose{};
    pose.targetObject = "00000000-0000-0000-0000-000000000042";
    pose.terrain = true;
    pose.observerMeters = {6'371'123.456789012345, -17.25, 4.0e-9};
    pose.surfaceFrame = {
        .east = {0.0, 0.0, 1.0},
        .north = {0.0, 1.0, 0.0},
        .up = {1.0, 0.0, 0.0}};
    pose.yawRadians = 1.2345678901234567;
    pose.pitchRadians = -0.5235987755982988;
    pose.zoom = 2.5;
    return {.pose = pose, .simulationMicroseconds = 86'400'000'000LL};
}

void PoseRoundTripsExactly()
{
    const auto original = Sample();
    const auto parsed =
        ParseViewContinuity(SerializeViewContinuity(original));

    Check(parsed.simulationMicroseconds == original.simulationMicroseconds,
          "simulation time round-trips");
    Check(parsed.pose.has_value(), "pose survives");
    const auto& a = *original.pose;
    const auto& b = *parsed.pose;
    Check(a.targetObject == b.targetObject, "target");
    Check(a.terrain == b.terrain && a.hasCamera == b.hasCamera, "flags");
    Check(a.observerMeters.x == b.observerMeters.x &&
              a.observerMeters.y == b.observerMeters.y &&
              a.observerMeters.z == b.observerMeters.z,
          "observer is bit-exact");
    Check(a.yawRadians == b.yawRadians && a.pitchRadians == b.pitchRadians,
          "look angles are bit-exact");
    Check(a.zoom == b.zoom, "zoom");
    Check(a.surfaceFrame.up.x == b.surfaceFrame.up.x &&
              a.surfaceFrame.east.z == b.surfaceFrame.east.z,
          "surface frame");
}

void PartialPoseIsRejectedWhole()
{
    auto text = SerializeViewContinuity(Sample());
    const auto at = text.find("pitch=");
    Check(at != std::string::npos, "sample has pitch");
    text.erase(at);

    const auto parsed = ParseViewContinuity(text);
    Check(!parsed.pose.has_value(), "a pose missing a field is dropped");
    Check(parsed.simulationMicroseconds.has_value(),
          "time still restores without the pose");
}

void NonFiniteAndDegenerateInputIsRejected()
{
    auto text = SerializeViewContinuity(Sample());
    const auto at = text.find("yaw=");
    text.replace(at, text.find('\n', at) - at, "yaw=nan");
    Check(!ParseViewContinuity(text).pose.has_value(), "nan yaw rejected");

    auto flat = SerializeViewContinuity(Sample());
    const auto up = flat.find("up=");
    flat.replace(up, flat.find('\n', up) - up, "up=0 0 0");
    Check(!ParseViewContinuity(flat).pose.has_value(),
          "zero-length frame axis rejected");

    Check(!ParseViewContinuity("").pose.has_value(), "empty text");
    Check(!ParseViewContinuity("garbage\n=\n").pose.has_value(), "garbage");
}

void TargetOnlyPoseRoundTrips()
{
    StudioViewContinuitySnapshot snapshot;
    StudioViewPose pose{};
    pose.targetObject = "body";
    pose.hasCamera = false;
    snapshot.pose = pose;

    const auto parsed =
        ParseViewContinuity(SerializeViewContinuity(snapshot));
    Check(parsed.pose.has_value() && !parsed.pose->hasCamera &&
              parsed.pose->targetObject == "body",
          "reference-sphere target without a camera");
}

void FileSaveAndLoad()
{
    const auto directory =
        std::filesystem::temp_directory_path() / "orbit_view_continuity_test";
    std::filesystem::remove_all(directory);
    const auto path = directory / ".orbit" / "StudioView.ini";

    Check(!LoadViewContinuity(path).pose.has_value(),
          "missing file yields nothing");

    SaveViewContinuity(path, Sample());
    Check(LoadViewContinuity(path).pose.has_value(), "saved file loads");
    Check(!std::filesystem::exists(path.string() + ".tmp"),
          "temporary file is replaced");

    std::filesystem::remove_all(directory);
}
} // namespace

int main()
{
    PoseRoundTripsExactly();
    PartialPoseIsRejectedWhole();
    NonFiniteAndDegenerateInputIsRejected();
    TargetOnlyPoseRoundTrips();
    FileSaveAndLoad();
    return EXIT_SUCCESS;
}

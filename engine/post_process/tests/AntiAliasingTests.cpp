#include <orbit/post_process/AntiAliasing.hpp>

#include <cmath>
#include <set>
#include <utility>

namespace
{
int failures = 0;

void Check(const bool condition)
{
    if (!condition)
    {
        ++failures;
    }
}

float Dot(const orbit::math::Float3& a, const orbit::math::Float3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
} // namespace

int main()
{
    using namespace orbit;
    using namespace orbit::post_process;

    // Mode names round-trip and unknown text is refused.
    {
        AntiAliasingMode mode = AntiAliasingMode::Off;
        Check(ParseAntiAliasingMode("TAA", mode) && mode == AntiAliasingMode::Taa);
        Check(ParseAntiAliasingMode("fxaa", mode) && mode == AntiAliasingMode::Fxaa);
        Check(ParseAntiAliasingMode("off", mode) && mode == AntiAliasingMode::Off);
        Check(!ParseAntiAliasingMode("msaa", mode));
        Check(AntiAliasingModeName(AntiAliasingMode::Taa) == "taa");
    }

    // The jitter sequence stays inside the pixel, repeats every 8 frames and
    // does not repeat a sample within the cycle.
    {
        std::set<std::pair<int, int>> seen;
        for (u32 frame = 0U; frame < 8U; ++frame)
        {
            const auto jitter = TaaJitterPixels(frame);
            Check(std::abs(jitter[0]) < 0.5F && std::abs(jitter[1]) < 0.5F);
            seen.insert({
                static_cast<int>(std::lround(jitter[0] * 1000.0F)),
                static_cast<int>(std::lround(jitter[1] * 1000.0F))});
            Check(TaaJitterPixels(frame + 8U) == jitter);
        }
        Check(seen.size() == 8U);
    }

    // A jittered camera is a rotation of about one pixel at most, keeps its
    // up vector perpendicular and unit length.
    {
        const math::Float3 forward{0.0F, 0.0F, 1.0F};
        const math::Float3 up{0.0F, 1.0F, 0.0F};
        math::Float3 jf;
        math::Float3 ju;
        ApplyCameraJitter(forward, up, 1.2217F, 1000U, {0.5F, -0.25F}, jf, ju);

        const float pixelTangent = 2.0F * std::tan(1.2217F * 0.5F) / 1000.0F;
        // +x jitter moves the view direction toward +right (right = forward x up).
        const float lateral = std::abs(jf.x);
        Check(std::abs(lateral - 0.5F * pixelTangent) < 1.0e-5F);
        Check(std::abs(Dot(jf, ju)) < 1.0e-5F);
        Check(std::abs(Dot(jf, jf) - 1.0F) < 1.0e-5F);
        Check(std::abs(Dot(ju, ju) - 1.0F) < 1.0e-5F);

        // Zero jitter leaves the camera alone.
        ApplyCameraJitter(forward, up, 1.2217F, 1000U, {0.0F, 0.0F}, jf, ju);
        Check(std::abs(jf.z - 1.0F) < 1.0e-6F && std::abs(ju.y - 1.0F) < 1.0e-6F);
    }

    // History is only reused across small, same-lens camera changes.
    {
        TaaCamera previous;
        previous.positionMeters = {1.0, 2.0, 3.0};
        TaaCamera current = previous;
        Check(TaaHistoryUsable(previous, current));

        current.positionMeters = {1.0, 2.0, 3.0 + 10.0};
        Check(TaaHistoryUsable(previous, current));

        current.positionMeters = {1.0, 2.0, 3.0 + 50000.0};
        Check(!TaaHistoryUsable(previous, current));

        current = previous;
        current.forward = {1.0F, 0.0F, 0.0F};
        Check(!TaaHistoryUsable(previous, current));

        current = previous;
        current.verticalFovRadians += 0.1F;
        Check(!TaaHistoryUsable(previous, current));
    }

    return failures;
}

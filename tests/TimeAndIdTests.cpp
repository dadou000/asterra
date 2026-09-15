#include <orbit/core/StrongId.hpp>
#include <orbit/time/SimulationClock.hpp>

#include <cassert>
#include <chrono>

namespace
{
struct TestIdTag;
using TestId = orbit::core::StrongId<TestIdTag>;
}

int main()
{
    const TestId known{
        .high = 0x0123456789abcdefULL,
        .low = 0xfedcba9876543210ULL
    };

    const std::string text =
        known.ToString();

    assert(
        text ==
        "01234567-89ab-cdef-fedc-ba9876543210");

    const auto parsed =
        TestId::Parse(text);

    assert(parsed.has_value());
    assert(*parsed == known);

    const auto compact =
        TestId::Parse(
            "0123456789abcdeffedcba9876543210");

    assert(compact.has_value());
    assert(*compact == known);

    assert(
        !TestId::Parse("not-an-id").has_value());

    const TestId random =
        TestId::Random();

    assert(random.IsValid());
    assert(
        TestId::Parse(random.ToString()) ==
        random);

    using namespace std::chrono_literals;

    orbit::time::SimulationClock clock({
        .epoch = {
            .microsecondsFromEpoch =
                1'000'000
        },
        .fixedStep = 10ms,
        .maximumFixedStepsPerAdvance = 8
    });

    auto advance =
        clock.Advance(25ms);

    assert(advance.fixedSteps == 2);
    assert(
        advance.fixedTime.microsecondsFromEpoch ==
        1'020'000);
    assert(
        advance.renderTime.microsecondsFromEpoch ==
        1'025'000);
    assert(
        advance.interpolationAlpha > 0.49 &&
        advance.interpolationAlpha < 0.51);

    clock.SetPaused(true);
    advance = clock.Advance(100ms);
    assert(advance.fixedSteps == 0);
    assert(
        advance.renderTime.microsecondsFromEpoch ==
        1'025'000);

    const auto stepped =
        clock.Step(2);
    assert(
        stepped.fixedTime.microsecondsFromEpoch ==
        1'040'000);
    assert(
        stepped.renderTime.microsecondsFromEpoch ==
        1'045'000);

    clock.SetPaused(false);
    clock.SetTimeScale(2.0);
    advance = clock.Advance(5ms);

    assert(advance.fixedSteps == 1);
    assert(
        advance.fixedTime.microsecondsFromEpoch ==
        1'050'000);
    assert(
        advance.renderTime.microsecondsFromEpoch ==
        1'055'000);

    return 0;
}

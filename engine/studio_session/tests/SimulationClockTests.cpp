#include <orbit/studio_session/SimulationClock.hpp>

#include <cmath>

int main()
{
    orbit::studio_session::SimulationClock clock;

    if (clock.Time().microsecondsFromEpoch != 0 ||
        clock.Playing() ||
        clock.Rate() != 1.0)
    {
        return 1;
    }

    clock.SetRate(60.0);
    clock.SetPlaying(true);
    clock.Advance(0.5);

    if (clock.Time().microsecondsFromEpoch !=
        30'000'000)
    {
        return 2;
    }

    clock.SetPlaying(false);
    clock.Advance(10.0);

    if (clock.Time().microsecondsFromEpoch !=
        30'000'000)
    {
        return 3;
    }

    clock.StepSeconds(-10.25);

    if (clock.Time().microsecondsFromEpoch !=
        19'750'000)
    {
        return 4;
    }

    clock.SetTime({
        .microsecondsFromEpoch = 123
    });

    if (clock.Time().microsecondsFromEpoch != 123)
    {
        return 5;
    }

    return 0;
}

#include <orbit/core/Log.hpp>
#include <orbit/platform/Window.hpp>

#include <exception>

int main()
{
    try
    {
        orbit::log::Info("Orbit M0 boot.");

        auto window = orbit::platform::CreateWindow({
            .title = "Orbit - Asterra Engine",
            .width = 1600,
            .height = 900
        });

        while (window->PumpEvents())
        {
            // Deliberately empty. Rendering, simulation, and editor systems
            // will live in independent modules rather than this application loop.
        }

        orbit::log::Info("Orbit shutdown.");
        return 0;
    }
    catch (const std::exception& exception)
    {
        orbit::log::Error(exception.what());
        return 1;
    }
}

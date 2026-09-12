#include <orbit/core/Log.hpp>
#include <orbit/platform/Window.hpp>
#include <orbit/rhi/d3d12/D3D12Backend.hpp>

#include <exception>
#include <format>

int main()
{
    try
    {
        orbit::log::Info("Orbit M0 boot.");

        auto window = orbit::platform::MakeWindow({
            .title = "Orbit - Asterra Engine",
            .width = 1600,
            .height = 900
        });

#if defined(NDEBUG)
        constexpr bool enableValidation = false;
#else
        constexpr bool enableValidation = true;
#endif

        auto device = orbit::rhi::d3d12::CreateDevice({
            .enableValidation = enableValidation
        });

        const auto& capabilities = device->Capabilities();

        orbit::log::Info(std::format(
            "GPU: {} | SM {}.{} | RT: {} | Mesh shaders: {} | VRS: {}",
            device->AdapterName(),
            capabilities.shaderModelMajor,
            capabilities.shaderModelMinor,
            capabilities.rayTracing,
            capabilities.meshShaders,
            capabilities.variableRateShading
        ));

        while (window->PumpEvents())
        {
            // Rendering, simulation, and editor systems remain independent modules.
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

#include <orbit/shader/dxc/DxcShaderCompiler.hpp>

#include "../src/PhysicalPageCompositeCompute.hpp"

#include <cstdlib>
#include <iostream>

int main()
{
    const orbit::shader::dxc::
        DxcShaderCompiler compiler;

    const auto binary =
        compiler.Compile({
            .source =
                orbit::terrain_gpu::detail::
                    kPhysicalPageCompositeShader,
            .entryPoint = "main",
            .stage =
                orbit::shader::Stage::Compute,
            .debug = false
        });

    if (binary.bytecode.empty() ||
        binary.stage !=
            orbit::shader::Stage::Compute)
    {
        std::cerr
            << "M12 physical-page composite shader failed compilation.\n";
        return EXIT_FAILURE;
    }

    std::cout
        << "Orbit M12 physical-page composite shader compiled successfully.\n";
    return EXIT_SUCCESS;
}

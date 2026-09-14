#pragma once

#include <orbit/shader/ShaderCompiler.hpp>

namespace orbit::shader::dxc
{
// Compiles the engine's embedded HLSL to SPIR-V via DXC, targeting
// Vulkan 1.3 (see engine/rhi/vulkan). Replaces the legacy D3DCompile
// (FXC, shader model 5.1) path, which cannot target SPIR-V at all.
class DxcShaderCompiler final : public Compiler
{
public:
    [[nodiscard]] Binary Compile(
        const CompileRequest& request) const override;
};
} // namespace orbit::shader::dxc

#pragma once

#include <orbit/shader/ShaderCompiler.hpp>

namespace orbit::shader::d3d
{
class D3DShaderCompiler final : public Compiler
{
public:
    [[nodiscard]] Binary Compile(
        const CompileRequest& request) const override;
};
} // namespace orbit::shader::d3d

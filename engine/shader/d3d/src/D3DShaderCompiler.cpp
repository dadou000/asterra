#include <orbit/shader/d3d/D3DShaderCompiler.hpp>

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <stdexcept>
#include <string>

namespace orbit::shader::d3d
{
namespace
{
using Microsoft::WRL::ComPtr;

[[nodiscard]] const char* ProfileForStage(
    const Stage stage)
{
    switch (stage)
    {
    case Stage::Vertex:
        return "vs_5_1";
    case Stage::Pixel:
        return "ps_5_1";
    case Stage::Compute:
        return "cs_5_1";
    }

    throw std::invalid_argument(
        "Orbit received an invalid shader stage.");
}
} // namespace

Binary D3DShaderCompiler::Compile(
    const CompileRequest& request) const
{
    if (request.source.empty())
    {
        throw std::invalid_argument(
            "Orbit cannot compile an empty shader source.");
    }

    if (request.entryPoint.empty())
    {
        throw std::invalid_argument(
            "Orbit shader entry point cannot be empty.");
    }

    const std::string entryPoint(request.entryPoint);

    UINT flags =
        D3DCOMPILE_ENABLE_STRICTNESS |
        D3DCOMPILE_WARNINGS_ARE_ERRORS;

    if (request.debug)
    {
        flags |=
            D3DCOMPILE_DEBUG |
            D3DCOMPILE_SKIP_OPTIMIZATION;
    }
    else
    {
        flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
    }

    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> diagnostics;

    const HRESULT result = D3DCompile(
        request.source.data(),
        request.source.size(),
        nullptr,
        nullptr,
        nullptr,
        entryPoint.c_str(),
        ProfileForStage(request.stage),
        flags,
        0,
        &bytecode,
        &diagnostics);

    if (FAILED(result))
    {
        std::string message =
            "Orbit failed to compile HLSL.";

        if (diagnostics && diagnostics->GetBufferSize() > 0)
        {
            const auto* text = static_cast<const char*>(
                diagnostics->GetBufferPointer());

            message += "\n";
            message.append(
                text,
                diagnostics->GetBufferSize());
        }

        throw std::runtime_error(message);
    }

    const auto* begin = static_cast<const u8*>(
        bytecode->GetBufferPointer());

    const auto* end =
        begin + bytecode->GetBufferSize();

    Binary binary{};
    binary.stage = request.stage;
    binary.bytecode.assign(begin, end);
    return binary;
}
} // namespace orbit::shader::d3d

#include <orbit/shader/dxc/DxcShaderCompiler.hpp>

#define NOMINMAX
// dxcapi.h needs full COM/OLE Automation support (IUnknown, BSTR,
// IStream) that WIN32_LEAN_AND_MEAN strips out of Windows.h -- unlike
// the D3D12 backend's headers, which get by without it.
#include <Windows.h>

#include <dxc/dxcapi.h>
#include <wrl/client.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace orbit::shader::dxc
{
namespace
{
using Microsoft::WRL::ComPtr;

[[nodiscard]] const wchar_t* ProfileForStage(const Stage stage)
{
    switch (stage)
    {
    case Stage::Vertex:
        return L"vs_6_0";
    case Stage::Pixel:
        return L"ps_6_0";
    case Stage::Compute:
        return L"cs_6_0";
    }

    throw std::invalid_argument(
        "Orbit received an invalid shader stage.");
}

// Every identifier this ever has to convert (entry points, shader
// profiles) is plain ASCII, so a byte-widening conversion is enough --
// no need for a real UTF-8 decode here.
[[nodiscard]] std::wstring WidenAscii(const std::string_view text)
{
    return {text.begin(), text.end()};
}
} // namespace

Binary DxcShaderCompiler::Compile(const CompileRequest& request) const
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

    ComPtr<IDxcUtils> utils;
    if (FAILED(DxcCreateInstance(
            CLSID_DxcUtils, IID_PPV_ARGS(&utils))))
    {
        throw std::runtime_error(
            "Orbit failed to create the DXC utils instance.");
    }

    ComPtr<IDxcCompiler3> compiler;
    if (FAILED(DxcCreateInstance(
            CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))))
    {
        throw std::runtime_error(
            "Orbit failed to create the DXC compiler instance.");
    }

    ComPtr<IDxcBlobEncoding> sourceBlob;
    if (FAILED(utils->CreateBlobFromPinned(
            request.source.data(),
            static_cast<UINT32>(request.source.size()),
            DXC_CP_UTF8,
            &sourceBlob)))
    {
        throw std::runtime_error(
            "Orbit failed to create a DXC source blob.");
    }

    const std::wstring entryPoint = WidenAscii(request.entryPoint);
    const std::wstring profile = ProfileForStage(request.stage);

    std::vector<LPCWSTR> arguments{
        L"-E", entryPoint.c_str(),
        L"-T", profile.c_str(),
        L"-spirv",
        L"-fspv-target-env=vulkan1.3",
        L"-HV", L"2021"
    };

    if (request.debug)
    {
        arguments.push_back(L"-Zi");
        arguments.push_back(L"-Od");
    }
    else
    {
        arguments.push_back(L"-O3");
    }

    const DxcBuffer sourceBuffer{
        .Ptr = sourceBlob->GetBufferPointer(),
        .Size = sourceBlob->GetBufferSize(),
        .Encoding = DXC_CP_UTF8
    };

    ComPtr<IDxcResult> result;
    if (FAILED(compiler->Compile(
            &sourceBuffer,
            arguments.data(),
            static_cast<UINT32>(arguments.size()),
            nullptr,
            IID_PPV_ARGS(&result))))
    {
        throw std::runtime_error(
            "Orbit failed to invoke the DXC compiler.");
    }

    HRESULT compileStatus = S_OK;
    result->GetStatus(&compileStatus);

    if (FAILED(compileStatus))
    {
        std::string message = "Orbit failed to compile HLSL to SPIR-V.";

        ComPtr<IDxcBlobUtf8> errors;
        if (SUCCEEDED(result->GetOutput(
                DXC_OUT_ERRORS,
                IID_PPV_ARGS(&errors),
                nullptr)) &&
            errors != nullptr &&
            errors->GetStringLength() > 0)
        {
            message += "\n";
            message.append(
                errors->GetStringPointer(),
                errors->GetStringLength());
        }

        throw std::runtime_error(message);
    }

    ComPtr<IDxcBlob> objectBlob;
    if (FAILED(result->GetOutput(
            DXC_OUT_OBJECT,
            IID_PPV_ARGS(&objectBlob),
            nullptr)) ||
        objectBlob == nullptr)
    {
        throw std::runtime_error(
            "Orbit DXC compilation produced no SPIR-V output.");
    }

    const auto* begin =
        static_cast<const u8*>(objectBlob->GetBufferPointer());
    const auto* end = begin + objectBlob->GetBufferSize();

    Binary binary{};
    binary.stage = request.stage;
    binary.bytecode.assign(begin, end);
    return binary;
}
} // namespace orbit::shader::dxc

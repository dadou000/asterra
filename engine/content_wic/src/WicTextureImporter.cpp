#include <orbit/content_wic/WicTextureImporter.hpp>

#include <orbit/content/AssetPipeline.hpp>
#include <orbit/content/RuntimeTexture.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace orbit::content_wic
{
namespace
{
using Microsoft::WRL::ComPtr;

void Check(
    const HRESULT result,
    const char* message)
{
    if (FAILED(result))
    {
        throw std::runtime_error(
            message);
    }
}

class ComApartment
{
public:
    ComApartment()
    {
        const HRESULT result =
            CoInitializeEx(
                nullptr,
                COINIT_MULTITHREADED);

        if (SUCCEEDED(result))
        {
            uninitialize_ = true;
            return;
        }

        if (result !=
            RPC_E_CHANGED_MODE)
        {
            throw std::runtime_error(
                "WIC importer failed to initialize COM.");
        }
    }

    ~ComApartment()
    {
        if (uninitialize_)
        {
            CoUninitialize();
        }
    }

private:
    bool uninitialize_{false};
};

[[nodiscard]] content::ImportOutput
ImportTexture(
    const content::ImportRequest& request)
{
    ComApartment apartment;

    ComPtr<IWICImagingFactory> factory;

    Check(
        CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory)),
        "WIC importer failed to create its imaging factory.");

    ComPtr<IWICBitmapDecoder> decoder;

    Check(
        factory->CreateDecoderFromFilename(
            request.sourcePath.c_str(),
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnDemand,
            &decoder),
        "WIC importer failed to open the source image.");

    ComPtr<IWICBitmapFrameDecode> frame;

    Check(
        decoder->GetFrame(
            0,
            &frame),
        "WIC importer failed to decode the first image frame.");

    UINT width = 0;
    UINT height = 0;

    Check(
        frame->GetSize(
            &width,
            &height),
        "WIC importer failed to read image dimensions.");

    if (width == 0 ||
        height == 0)
    {
        throw std::runtime_error(
            "WIC importer received an empty image.");
    }

    ComPtr<IWICFormatConverter> converter;

    Check(
        factory->CreateFormatConverter(
            &converter),
        "WIC importer failed to create an RGBA converter.");

    Check(
        converter->Initialize(
            frame.Get(),
            GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom),
        "WIC importer cannot convert this image to RGBA8.");

    constexpr u64 bytesPerPixel = 4;

    const u64 stride64 =
        static_cast<u64>(
            width) *
        bytesPerPixel;

    const u64 byteCount64 =
        stride64 *
        static_cast<u64>(
            height);

    if (stride64 >
            std::numeric_limits<UINT>::
                max() ||
        byteCount64 >
            std::numeric_limits<UINT>::
                max())
    {
        throw std::runtime_error(
            "WIC texture exceeds the current Orbit importer size limit.");
    }

    std::vector<std::byte> rgba(
        static_cast<std::size_t>(
            byteCount64));

    Check(
        converter->CopyPixels(
            nullptr,
            static_cast<UINT>(
                stride64),
            static_cast<UINT>(
                byteCount64),
            reinterpret_cast<BYTE*>(
                rgba.data())),
        "WIC importer failed while copying RGBA pixels.");

    return content::ImportOutput{
        .artifacts = {
            {
                .name =
                    "texture.orbittex",
                .bytes =
                    content::
                        EncodeRuntimeTextureRgba8(
                            static_cast<u32>(
                                width),
                            static_cast<u32>(
                                height),
                            std::span(
                                rgba.data(),
                                rgba.size()))
            }
        }
    };
}
} // namespace

void RegisterTextureImporters(
    content::ImporterRegistry& registry)
{
    registry.Register({
        .id =
            "orbit.texture.wic_rgba8",
        .version = 1,
        .extensions = {
            ".png",
            ".jpg",
            ".jpeg",
            ".bmp"
        },
        .import =
            [](const content::ImportRequest&
                   request)
            {
                return ImportTexture(
                    request);
            }
    });
}
} // namespace orbit::content_wic

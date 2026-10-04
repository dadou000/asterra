#include <orbit/render_view/Capture.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#endif

namespace orbit::render_view
{
namespace
{
[[nodiscard]] std::string LowerExtension(const std::filesystem::path& path)
{
    std::string extension = path.extension().string();
    std::ranges::transform(
        extension,
        extension.begin(),
        [](const unsigned char c)
        {
            return static_cast<char>(std::tolower(c));
        });
    return extension;
}

[[nodiscard]] std::filesystem::path AbsoluteForWrite(
    const std::filesystem::path& path)
{
    if (path.empty())
    {
        throw std::invalid_argument("Image path must not be empty.");
    }
    std::filesystem::path absolute =
        path.is_relative() ? std::filesystem::absolute(path) : path;
    if (const auto parent = absolute.parent_path(); !parent.empty())
    {
        std::filesystem::create_directories(parent);
    }
    return absolute;
}

#if defined(_WIN32)
using Microsoft::WRL::ComPtr;

void Check(const HRESULT result, const char* message)
{
    if (FAILED(result))
    {
        throw std::runtime_error(message);
    }
}

// WIC needs COM on the calling thread; Studio's main thread may already be in a
// different apartment, which is fine.
class ComApartment
{
public:
    ComApartment()
    {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(result))
        {
            uninitialize_ = true;
            return;
        }
        if (result != RPC_E_CHANGED_MODE)
        {
            throw std::runtime_error("Image codec failed to initialize COM.");
        }
    }

    ~ComApartment()
    {
        if (uninitialize_)
        {
            CoUninitialize();
        }
    }

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

private:
    bool uninitialize_{false};
};
#endif
} // namespace

CaptureResult WritePngRgba8(
    const std::filesystem::path& path,
    const u32 width,
    const u32 height,
    const u8* const rgba)
{
    if (width == 0U || height == 0U || rgba == nullptr)
    {
        throw std::invalid_argument("PNG image is empty.");
    }
    const std::filesystem::path absolute = AbsoluteForWrite(path);

#if defined(_WIN32)
    {
        ComApartment apartment;

        ComPtr<IWICImagingFactory> factory;
        Check(
            CoCreateInstance(
                CLSID_WICImagingFactory,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&factory)),
            "Could not create the image factory.");

        ComPtr<IWICStream> stream;
        Check(factory->CreateStream(&stream), "Could not create the image stream.");
        Check(
            stream->InitializeFromFilename(absolute.c_str(), GENERIC_WRITE),
            "Could not open the screenshot destination.");

        ComPtr<IWICBitmapEncoder> encoder;
        Check(
            factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder),
            "Could not create the PNG encoder.");
        Check(
            encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache),
            "Could not start the PNG encoder.");

        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> properties;
        Check(
            encoder->CreateNewFrame(&frame, &properties),
            "Could not create the PNG frame.");
        Check(frame->Initialize(properties.Get()), "Could not start the PNG frame.");
        Check(frame->SetSize(width, height), "Could not size the PNG frame.");

        // 24-bit: the display target's alpha carries nothing, and a stray zero
        // would make the PNG transparent.
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        Check(frame->SetPixelFormat(&format), "Could not set the PNG pixel format.");
        if (format != GUID_WICPixelFormat24bppBGR)
        {
            throw std::runtime_error("The PNG encoder does not take 24-bit BGR.");
        }

        constexpr u32 kRowsPerChunk = 64U;
        const u32 stride = width * 3U;
        std::vector<BYTE> chunk(static_cast<std::size_t>(stride) * kRowsPerChunk);
        for (u32 row = 0U; row < height; row += kRowsPerChunk)
        {
            const u32 rows = std::min(kRowsPerChunk, height - row);
            for (u32 line = 0U; line < rows; ++line)
            {
                const u8* source = rgba +
                    (static_cast<std::size_t>(row + line) * width) * 4U;
                BYTE* out = chunk.data() + static_cast<std::size_t>(line) * stride;
                for (u32 x = 0U; x < width; ++x)
                {
                    out[x * 3U + 0U] = source[x * 4U + 2U];
                    out[x * 3U + 1U] = source[x * 4U + 1U];
                    out[x * 3U + 2U] = source[x * 4U + 0U];
                }
            }
            Check(
                frame->WritePixels(
                    rows,
                    stride,
                    static_cast<UINT>(static_cast<std::size_t>(stride) * rows),
                    chunk.data()),
                "Could not write PNG pixels.");
        }

        Check(frame->Commit(), "Could not finish the PNG frame.");
        Check(encoder->Commit(), "Could not finish the PNG file.");
    }

    std::error_code error;
    const auto size = std::filesystem::file_size(absolute, error);
    return {
        .path = absolute,
        .width = width,
        .height = height,
        .fileBytes = error ? 0U : static_cast<u64>(size)};
#else
    throw std::runtime_error("PNG output is only implemented for Windows.");
#endif
}

CapturedImage ReadImageRgba8(const std::filesystem::path& path)
{
#if defined(_WIN32)
    ComApartment apartment;

    ComPtr<IWICImagingFactory> factory;
    Check(
        CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory)),
        "Could not create the image factory.");

    ComPtr<IWICBitmapDecoder> decoder;
    Check(
        factory->CreateDecoderFromFilename(
            path.c_str(),
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnDemand,
            &decoder),
        "Could not open the image.");

    ComPtr<IWICBitmapFrameDecode> frame;
    Check(decoder->GetFrame(0, &frame), "Could not read the image frame.");

    ComPtr<IWICFormatConverter> converter;
    Check(factory->CreateFormatConverter(&converter), "Could not create the converter.");
    Check(
        converter->Initialize(
            frame.Get(),
            GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom),
        "Could not convert the image.");

    UINT width = 0U;
    UINT height = 0U;
    Check(converter->GetSize(&width, &height), "Could not read the image size.");

    CapturedImage image;
    image.width = width;
    image.height = height;
    image.rgba.resize(static_cast<std::size_t>(width) * height * 4U);
    Check(
        converter->CopyPixels(
            nullptr,
            width * 4U,
            static_cast<UINT>(image.rgba.size()),
            image.rgba.data()),
        "Could not copy the image pixels.");
    return image;
#else
    static_cast<void>(path);
    throw std::runtime_error("Image reading is only implemented for Windows.");
#endif
}

CaptureResult WriteImageRgba8(
    const std::filesystem::path& path,
    const u32 width,
    const u32 height,
    const u8* const rgba)
{
    if (LowerExtension(path) == ".bmp")
    {
        return WriteBmpRgba8(path, width, height, rgba);
    }
    return WritePngRgba8(path, width, height, rgba);
}

CaptureResult CaptureImageFile(
    rhi::Device& device,
    rhi::Queue& graphicsQueue,
    RenderView& view,
    const std::filesystem::path& path)
{
    if (LowerExtension(path) == ".bmp")
    {
        return CaptureBmp(device, graphicsQueue, view, path);
    }
    const CapturedImage image = CaptureRgba8(device, graphicsQueue, view);
    return WritePngRgba8(path, image.width, image.height, image.rgba.data());
}
} // namespace orbit::render_view

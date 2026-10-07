#include "StudioViewportInternals.hpp"

namespace orbit::studio_ui
{
using namespace viewport_detail;

void StudioViewportRenderer::SetColorLut(
    post_process::ColorLutData lut)
{
    if (device_ == nullptr)
    {
        throw std::logic_error(
            "Studio viewport renderer has no device for LUT replacement.");
    }

    if (!post_process::
            IsDisplayLutCompatible(
                lut))
    {
        throw std::invalid_argument(
            "Selected LUT is not compatible with the display-linear correction stage.");
    }

    colorLut_ =
        std::make_unique<
            post_process::GpuColorLut>(
                *device_,
                lut);
    colorLutData_ =
        std::move(lut);
    colorLutSourcePath_ =
        colorLutData_.metadata.title ==
                "Orbit Identity"
            ? "<identity>"
            : "<runtime>";
    colorLutDiagnostic_.clear();
}

std::vector<std::string>
StudioViewportRenderer::ColorLutAssetPaths() const
{
    std::vector<std::string> result;

    if (content_ == nullptr)
    {
        return result;
    }

    for (const auto& asset :
         content_->Search(
             {},
             content::AssetKind::ColorLut))
    {
        result.push_back(
            asset.sourcePath.
                generic_string());
    }

    return result;
}

bool StudioViewportRenderer::SelectColorLutAsset(
    const std::string_view projectRelativePath)
{
    colorLutDiagnostic_.clear();

    if (content_ == nullptr)
    {
        colorLutDiagnostic_ =
            "Content service is unavailable.";
        return false;
    }

    const auto* asset =
        content_->FindByPath(
            std::filesystem::path(
                projectRelativePath));

    if (asset == nullptr ||
        asset->kind !=
            content::AssetKind::ColorLut)
    {
        colorLutDiagnostic_ =
            "Selected path is not an indexed .cube LUT asset.";
        return false;
    }

    try
    {
        std::ifstream stream{
            content_->AbsolutePath(
                asset->id),
            std::ios::binary};

        if (!stream)
        {
            throw std::runtime_error(
                "Unable to open LUT asset.");
        }

        std::ostringstream text;
        text << stream.rdbuf();

        auto imported =
            post_process::
                ParseCubeColorLut(
                    text.str());

        if (!post_process::
                IsDisplayLutCompatible(
                    imported.lut))
        {
            colorLutDiagnostic_ =
                "Rejected LUT: Display correction accepts only DisplayLinear / None-shaper assets. Imported metadata is " +
                std::string(
                    post_process::
                        ColorLutDomainName(
                            imported.lut.metadata.domain)) +
                " / " +
                std::string(
                    post_process::
                        ColorLutShaperName(
                            imported.lut.metadata.shaper)) +
                ".";
            return false;
        }

        SetColorLut(
            std::move(imported.lut));
        colorLutSourcePath_ =
            asset->sourcePath.
                generic_string();
        return true;
    }
    catch (const std::exception& exception)
    {
        colorLutDiagnostic_ =
            exception.what();
        return false;
    }
}

bool StudioViewportRenderer::ImportColorLutFile(
    const std::string_view sourcePath)
{
    colorLutDiagnostic_.clear();

    if (content_ == nullptr)
    {
        colorLutDiagnostic_ =
            "Content service is unavailable.";
        return false;
    }

    try
    {
        const auto importedId =
            content_->ImportFile(
                std::filesystem::path(
                    sourcePath));

        const auto* asset =
            content_->Find(
                importedId);

        if (asset == nullptr ||
            asset->kind !=
                content::AssetKind::ColorLut)
        {
            colorLutDiagnostic_ =
                "Imported file is not a supported .cube LUT.";
            return false;
        }

        return SelectColorLutAsset(
            asset->sourcePath.
                generic_string());
    }
    catch (const std::exception& exception)
    {
        colorLutDiagnostic_ =
            exception.what();
        return false;
    }
}

StudioColorLutDiagnostics
StudioViewportRenderer::ColorLutDiagnostics() const
{
    return {
        .sourcePath =
            colorLutSourcePath_,
        .title =
            colorLutData_.metadata.title,
        .size =
            colorLutData_.size,
        .domain =
            colorLutData_.metadata.domain,
        .shaper =
            colorLutData_.metadata.shaper,
        .compatible =
            post_process::
                IsDisplayLutCompatible(
                    colorLutData_),
        .explicitMetadata =
            colorLutData_.metadata.
                explicitOrbitMetadata,
        .diagnostic =
            colorLutDiagnostic_
    };
}

post_process::ColorLutSettings
StudioViewportRenderer::ColorLutSettings() const noexcept
{
    return colorLutSettings_;
}

void StudioViewportRenderer::SetColorLutSettings(
    post_process::ColorLutSettings settings) noexcept
{
    settings.strength =
        std::clamp(
            settings.strength,
            0.0F,
            1.0F);
    colorLutSettings_ = settings;
}

StudioOutputTransformDiagnostics
StudioViewportRenderer::OutputTransformDiagnostics() const noexcept
{
    return {
        .settings =
            outputTransformSettings_,
        .capabilities =
            outputDisplayCapabilities_,
        .resolved =
            post_process::
                ResolveOutputTransform(
                    outputTransformSettings_,
                    outputDisplayCapabilities_)
    };
}

void StudioViewportRenderer::SetOutputTransformSettings(
    post_process::OutputTransformSettings settings) noexcept
{
    settings.referenceWhiteNits =
        std::max(
            settings.referenceWhiteNits,
            1.0F);
    settings.requestedPeakNits =
        std::max(
            settings.requestedPeakNits,
            settings.referenceWhiteNits);

    outputTransformSettings_ =
        settings;
}

void StudioViewportRenderer::SetOutputDisplayCapabilities(
    post_process::OutputDisplayCapabilities capabilities) noexcept
{
    capabilities.reportedPeakNits =
        std::max(
            capabilities.reportedPeakNits,
            0.0F);

    outputDisplayCapabilities_ =
        capabilities;
}

void StudioViewportRenderer::SetDisplayResolveSettings(
    post_process::DisplayResolveSettings settings) noexcept
{
    settings.exposureScale =
        std::max(
            settings.exposureScale,
            0.0F);
    displayResolveSettings_ = settings;
}

} // namespace orbit::studio_ui

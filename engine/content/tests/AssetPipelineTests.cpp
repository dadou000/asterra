#include <orbit/content/AssetPipeline.hpp>
#include <orbit/content/ContentHash.hpp>
#include <orbit/content/DerivedDataCache.hpp>
#include <orbit/content/ThumbnailService.hpp>
#include <orbit/core/StrongId.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <string>
#include <vector>

namespace
{
void Check(
    const bool condition,
    const std::source_location location =
        std::source_location::current())
{
    if (!condition)
    {
        std::cerr
            << "AssetPipeline test failed at "
            << location.file_name()
            << ':'
            << location.line()
            << '\n';
        std::exit(1);
    }
}

void Write(
    const std::filesystem::path& path,
    const std::string_view text)
{
    std::filesystem::create_directories(
        path.parent_path());

    std::ofstream output(
        path,
        std::ios::binary |
        std::ios::trunc);

    Check(output.good());
    output << text;
}

[[nodiscard]] std::vector<std::byte>
Bytes(const std::string_view text)
{
    const auto bytes =
        std::as_bytes(
            std::span(
                text.data(),
                text.size()));

    return {
        bytes.begin(),
        bytes.end()
    };
}
} // namespace

int main()
{
    Check(
        orbit::content::HashString("abc").
            ToHex() ==
        "ba7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad");

    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-asset-pipeline-" +
         orbit::core::StrongId<
             struct PipelineTestTag>::
             Random().
             ToString());

    std::filesystem::remove_all(
        root);

    std::filesystem::create_directories(
        root / "Content");

    const auto source =
        root / "Content" / "vehicle.asset";
    const auto dependency =
        root / "Content" / "shared.dep";

    Write(
        source,
        "source-v1");
    Write(
        dependency,
        "dependency-v1");

    orbit::content::ImporterRegistry
        importers;

    int importCalls = 0;

    importers.Register({
        .id = "test.asset",
        .version = 1,
        .extensions = {".asset"},
        .import =
            [&importCalls](
                const orbit::content::
                    ImportRequest& request)
            {
                ++importCalls;

                std::ifstream sourceInput(
                    request.sourcePath,
                    std::ios::binary);

                std::string sourceText(
                    (std::istreambuf_iterator<char>(
                         sourceInput)),
                    std::istreambuf_iterator<char>());

                const std::string derived =
                    sourceText +
                    "|" +
                    request.settings +
                    "|" +
                    request.targetPlatform;

                return orbit::content::ImportOutput{
                    .artifacts = {
                        {
                            .name = "runtime.bin",
                            .bytes =
                                Bytes(derived)
                        }
                    },
                    .dependencies = {
                        "shared.dep"
                    }
                };
            }
    });

    Check(
        importers.Catalog().size() == 1);
    Check(
        importers.FindFor(source) !=
        nullptr);

    bool duplicateRejected = false;

    try
    {
        importers.Register({
            .id = "other.asset",
            .version = 1,
            .extensions = {".asset"},
            .import =
                [](
                    const orbit::content::
                        ImportRequest&)
                {
                    return orbit::content::
                        ImportOutput{};
                }
        });
    }
    catch (const std::invalid_argument&)
    {
        duplicateRejected = true;
    }

    Check(duplicateRejected);

    orbit::content::DerivedDataCache cache(
        root /
        ".orbit" /
        "DerivedData");

    orbit::content::AssetPipeline pipeline(
        importers,
        cache);

    const auto first =
        pipeline.Import(
            root,
            source,
            "quality=high",
            "windows");

    Check(!first.cacheHit);
    Check(importCalls == 1);
    Check(first.artifacts.size() == 1);
    Check(first.dependencies.size() == 1);
    Check(
        cache.Contains(
            first.key,
            "runtime.bin"));

    const auto firstBytes =
        cache.Read(
            first.key,
            "runtime.bin");

    Check(firstBytes.has_value());
    Check(
        std::string(
            reinterpret_cast<const char*>(
                firstBytes->data()),
            firstBytes->size()) ==
        "source-v1|quality=high|windows");

    const auto second =
        pipeline.Import(
            root,
            source,
            "quality=high",
            "windows");

    Check(second.cacheHit);
    Check(importCalls == 1);
    Check(second.key == first.key);

    Write(
        dependency,
        "dependency-v2");

    const auto dependencyChanged =
        pipeline.Import(
            root,
            source,
            "quality=high",
            "windows");

    Check(!dependencyChanged.cacheHit);
    Check(importCalls == 2);
    Check(
        dependencyChanged.key ==
        first.key);

    const auto afterDependency =
        pipeline.Import(
            root,
            source,
            "quality=high",
            "windows");

    Check(afterDependency.cacheHit);
    Check(importCalls == 2);

    const auto settingsChanged =
        pipeline.Import(
            root,
            source,
            "quality=low",
            "windows");

    Check(!settingsChanged.cacheHit);
    Check(importCalls == 3);
    Check(
        settingsChanged.key !=
        first.key);

    Write(
        source,
        "source-v2");

    const auto sourceChanged =
        pipeline.Import(
            root,
            source,
            "quality=high",
            "windows");

    Check(!sourceChanged.cacheHit);
    Check(importCalls == 4);
    Check(
        sourceChanged.key !=
        first.key);

    orbit::content::ImporterRegistry
        versionTwoImporters;

    int versionTwoCalls = 0;

    versionTwoImporters.Register({
        .id = "test.asset",
        .version = 2,
        .extensions = {".asset"},
        .import =
            [&versionTwoCalls](
                const orbit::content::
                    ImportRequest&)
            {
                ++versionTwoCalls;

                return orbit::content::ImportOutput{
                    .artifacts = {
                        {
                            .name = "runtime.bin",
                            .bytes =
                                Bytes("version-two")
                        }
                    },
                    .dependencies = {
                        "shared.dep"
                    }
                };
            }
    });

    orbit::content::AssetPipeline
        versionTwoPipeline(
            versionTwoImporters,
            cache);

    const auto versionChanged =
        versionTwoPipeline.Import(
            root,
            source,
            "quality=high",
            "windows");

    Check(!versionChanged.cacheHit);
    Check(versionTwoCalls == 1);
    Check(
        versionChanged.key !=
        sourceChanged.key);

    std::filesystem::remove_all(
        cache.Root());

    orbit::content::DerivedDataCache rebuiltCache(
        root /
        ".orbit" /
        "DerivedData");

    orbit::content::AssetPipeline rebuiltPipeline(
        versionTwoImporters,
        rebuiltCache);

    const auto afterDelete =
        rebuiltPipeline.Import(
            root,
            source,
            "quality=high",
            "windows");

    Check(!afterDelete.cacheHit);
    Check(versionTwoCalls == 2);

    Check(
        std::filesystem::is_regular_file(
            source));
    Check(
        std::filesystem::is_regular_file(
            dependency));

    orbit::content::ThumbnailService
        thumbnails(
            rebuiltCache);

    const auto fallback =
        thumbnails.Get({
            .sourceHash =
                orbit::content::HashFile(
                    source),
            .category = "Mesh",
            .label = "Vehicle",
            .width = 48,
            .height = 32
        });

    Check(!fallback.cacheHit);
    Check(
        fallback.providerId ==
        "orbit.fallback");
    Check(
        std::filesystem::is_regular_file(
            fallback.path));

    const auto fallbackCached =
        thumbnails.Get({
            .sourceHash =
                orbit::content::HashFile(
                    source),
            .category = "Mesh",
            .label = "Vehicle",
            .width = 48,
            .height = 32
        });

    Check(fallbackCached.cacheHit);
    Check(
        fallbackCached.key ==
        fallback.key);

    int thumbnailProviderCalls = 0;

    thumbnails.RegisterProvider({
        .id = "test.mesh.thumbnail",
        .version = 1,
        .category = "Mesh",
        .generate =
            [&thumbnailProviderCalls](
                const orbit::content::
                    ThumbnailRequest& request)
            {
                ++thumbnailProviderCalls;

                return orbit::content::
                    ThumbnailPixels{
                        .width =
                            request.width,
                        .height =
                            request.height,
                        .rgba8 =
                            std::vector<std::byte>(
                                static_cast<std::size_t>(
                                    request.width) *
                                request.height *
                                4U,
                                std::byte{0x7f})
                    };
            }
    });

    const auto specialized =
        thumbnails.Get({
            .sourceHash =
                orbit::content::HashFile(
                    source),
            .category = "Mesh",
            .label = "Vehicle",
            .width = 48,
            .height = 32
        });

    Check(!specialized.cacheHit);
    Check(thumbnailProviderCalls == 1);
    Check(
        specialized.providerId ==
        "test.mesh.thumbnail");
    Check(
        specialized.key !=
        fallback.key);

    const auto specializedCached =
        thumbnails.Get({
            .sourceHash =
                orbit::content::HashFile(
                    source),
            .category = "Mesh",
            .label = "Vehicle",
            .width = 48,
            .height = 32
        });

    Check(specializedCached.cacheHit);
    Check(thumbnailProviderCalls == 1);

    std::filesystem::remove_all(
        root);

    return 0;
}

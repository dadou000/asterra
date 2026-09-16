#include <orbit/build/BuildService.hpp>
#include <orbit/documents/ProjectDocument.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#define ORBIT_TEST_CHECK(expression) \
    do \
    { \
        if (!(expression)) \
        { \
            std::cerr \
                << "BuildService test failed: " \
                << #expression \
                << " at line " \
                << __LINE__ \
                << '\n'; \
            return 1; \
        } \
    } while (false)

namespace
{
void WriteText(
    const std::filesystem::path& path,
    const std::string_view text)
{
    std::filesystem::create_directories(
        path.parent_path());

    std::ofstream output(
        path,
        std::ios::binary |
            std::ios::trunc);

    if (!output)
    {
        throw std::runtime_error(
            "Unable to create test file: " +
            path.string());
    }

    output << text;

    if (!output)
    {
        throw std::runtime_error(
            "Unable to write test file: " +
            path.string());
    }
}

void WriteTinyBmp(
    const std::filesystem::path& path)
{
    constexpr std::array<
        unsigned char,
        58>
        bytes{
            0x42, 0x4d,
            0x3a, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00,
            0x36, 0x00, 0x00, 0x00,
            0x28, 0x00, 0x00, 0x00,
            0x01, 0x00, 0x00, 0x00,
            0x01, 0x00, 0x00, 0x00,
            0x01, 0x00,
            0x20, 0x00,
            0x00, 0x00, 0x00, 0x00,
            0x04, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00,
            0x20, 0x40, 0x80, 0xff
        };

    std::filesystem::create_directories(
        path.parent_path());

    std::ofstream output(
        path,
        std::ios::binary |
            std::ios::trunc);

    output.write(
        reinterpret_cast<const char*>(
            bytes.data()),
        static_cast<std::streamsize>(
            bytes.size()));

    if (!output)
    {
        throw std::runtime_error(
            "Unable to write test BMP.");
    }
}

[[nodiscard]] std::string ReadText(
    const std::filesystem::path& path)
{
    std::ifstream input(
        path,
        std::ios::binary);

    if (!input)
    {
        throw std::runtime_error(
            "Unable to read test file: " +
            path.string());
    }

    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

class TemporaryProject
{
public:
    TemporaryProject()
        : root_(
              std::filesystem::
                  temp_directory_path() /
              ("orbit-build-test-" +
               orbit::documents::
                   ProjectId::Random().
                   ToString()))
    {
    }

    ~TemporaryProject()
    {
        std::error_code ignored;
        std::filesystem::remove_all(
            root_,
            ignored);
    }

    [[nodiscard]] const std::filesystem::path&
    Root() const noexcept
    {
        return root_;
    }

private:
    std::filesystem::path root_;
};
} // namespace

int main()
{
    try
    {
        TemporaryProject temporary;

        auto project =
            orbit::documents::
                ProjectDocument::Create(
                    temporary.Root(),
                    "Build Service Test");

        WriteText(
            temporary.Root() /
                "Scripts/main.luau",
            "local value = 40 + 2\nreturn value\n");

        WriteText(
            temporary.Root() /
                "Scripts/vehicle/control.luau",
            "local M = {}\nfunction M.update() return 1 end\nreturn M\n");

        WriteText(
            temporary.Root() /
                "Plugins/test.plugin/plugin.toml",
            "[plugin]\n"
            "id = \"test.plugin\"\n"
            "version = \"1.0.0\"\n"
            "orbit_api = \"0.0.3\"\n"
            "entry = \"Scripts/main.luau\"\n"
            "scope = \"both\"\n");

        WriteText(
            temporary.Root() /
                "Plugins/test.plugin/Scripts/main.luau",
            "return {}\n");

        WriteTinyBmp(
            temporary.Root() /
                "Content/Test.bmp");

        WriteText(
            temporary.Root() /
                "Content/Test.orbitmaterial",
            "[material]\n"
            "name = \"Test Material\"\n"
            "base_color = \"Test.bmp\"\n"
            "roughness_factor = 0.75\n"
            "metallic_factor = 0.0\n");


        WriteText(
            temporary.Root() /
                "Content/Shaders/Test.hlsl",
            "[numthreads(1, 1, 1)]\n"
            "void CSMain(uint3 id : SV_DispatchThreadID) {}\n");

        WriteText(
            temporary.Root() /
                "Content/Shaders/Test.hlsl.orbitshader.toml",
            "[shader]\n"
            "stage = \"compute\"\n"
            "entry = \"CSMain\"\n");

        project.Manifest().
            scriptEntryPoints = {
                "Scripts/main.luau"
            };
        project.Manifest().
            plugins.push_back({
                .id = "test.plugin",
                .version = "1.0.0"
            });
        project.Save();

        orbit::build::BuildService service;

        orbit::build::BuildRequest request{
            .manifestPath =
                project.ManifestPath(),
            .profileName =
                "Development Windows"
        };

        const auto validation =
            service.Validate(
                request);

        ORBIT_TEST_CHECK(
            validation.Succeeded());
        ORBIT_TEST_CHECK(
            validation.profile.platform ==
            "Windows");
        ORBIT_TEST_CHECK(
            validation.profile.storefront ==
            "standalone");

        const auto first =
            service.Cook(
                request);

        ORBIT_TEST_CHECK(
            first.Succeeded());
        ORBIT_TEST_CHECK(
            std::filesystem::
                is_regular_file(
                    first.manifestPath));
        ORBIT_TEST_CHECK(
            std::filesystem::
                is_regular_file(
                    first.outputDirectory /
                    ".orbit-build-output"));
        ORBIT_TEST_CHECK(
            std::filesystem::
                is_regular_file(
                    first.outputDirectory /
                    "Worlds/Main.orbitworld"));
        ORBIT_TEST_CHECK(
            std::filesystem::
                is_regular_file(
                    first.outputDirectory /
                    "Scripts/main.luauc"));
        ORBIT_TEST_CHECK(
            first.manifest.assets.size() ==
            3U);
        ORBIT_TEST_CHECK(
            first.manifest.scripts.size() ==
            2U);
        ORBIT_TEST_CHECK(
            first.manifest.
                scriptEntryPoints.size() ==
            1U);
        ORBIT_TEST_CHECK(
            std::filesystem::
                is_regular_file(
                    first.outputDirectory /
                    "Scripts/vehicle/control.luauc"));
        ORBIT_TEST_CHECK(
            !first.manifest.assets.front().
                artifacts.empty());

        const std::string firstManifest =
            ReadText(
                first.manifestPath);

        std::filesystem::remove_all(
            temporary.Root() /
                ".orbit/DerivedData");

        const auto second =
            service.Cook(
                request);

        ORBIT_TEST_CHECK(
            second.Succeeded());

        const std::string secondManifest =
            ReadText(
                second.manifestPath);

        ORBIT_TEST_CHECK(
            firstManifest ==
            secondManifest);

        const auto warmCache =
            service.Cook(
                request);

        ORBIT_TEST_CHECK(
            warmCache.Succeeded());
        ORBIT_TEST_CHECK(
            ReadText(
                warmCache.manifestPath) ==
            firstManifest);

        const auto unownedOutput =
            temporary.Root() /
            "UnownedOutput";

        WriteText(
            unownedOutput /
                "keep.txt",
            "must survive\n");

        orbit::build::BuildRequest
            unownedRequest = request;

        unownedRequest.outputDirectory =
            unownedOutput;

        const auto unownedResult =
            service.Cook(
                unownedRequest);

        ORBIT_TEST_CHECK(
            !unownedResult.Succeeded());
        ORBIT_TEST_CHECK(
            std::filesystem::
                is_regular_file(
                    unownedOutput /
                    "keep.txt"));

        const auto dummyPlayer =
            temporary.Root() /
            "TestRuntime/OrbitPlayer.exe";

        WriteText(
            dummyPlayer,
            "orbit-player-test-binary");

        const auto package =
            service.Package(
                request,
                {
                    .playerExecutable =
                        dummyPlayer
                });

        ORBIT_TEST_CHECK(
            package.Succeeded());
        ORBIT_TEST_CHECK(
            std::filesystem::
                is_regular_file(
                    package.
                        executablePath));
        ORBIT_TEST_CHECK(
            std::filesystem::
                is_regular_file(
                    package.
                        packageManifestPath));
        ORBIT_TEST_CHECK(
            package.executablePath.
                parent_path() ==
            package.outputDirectory);

        orbit::build::BuildRequest
            protectedRequest = request;

        protectedRequest.outputDirectory =
            temporary.Root() /
            "Content";

        const auto protectedValidation =
            service.Validate(
                protectedRequest);

        ORBIT_TEST_CHECK(
            !protectedValidation.Succeeded());

        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr
            << "BuildService test threw: "
            << exception.what()
            << '\n';
        return 1;
    }
}

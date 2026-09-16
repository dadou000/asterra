#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <orbit/documents/ProjectDocument.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#define ORBIT_TEST_CHECK(expression) \
    do \
    { \
        if (!(expression)) \
        { \
            std::cerr \
                << "OrbitBuild CLI test failed: " \
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
            "Unable to create CLI test file: " +
            path.string());
    }

    output << text;

    if (!output)
    {
        throw std::runtime_error(
            "Unable to write CLI test file: " +
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
            "Unable to write CLI test BMP.");
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
            "Unable to read CLI test file: " +
            path.string());
    }

    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

[[nodiscard]] std::wstring Quote(
    const std::filesystem::path& path)
{
    return L"\"" +
        path.wstring() +
        L"\"";
}

[[nodiscard]] DWORD RunProcess(
    const std::filesystem::path& executable,
    const std::vector<std::wstring>&
        arguments)
{
    std::wstring command =
        Quote(executable);

    for (const auto& argument :
         arguments)
    {
        command += L" ";
        command += argument;
    }

    std::vector<wchar_t> mutableCommand(
        command.begin(),
        command.end());
    mutableCommand.push_back(
        L'\0');

    STARTUPINFOW startup{};
    startup.cb =
        sizeof(startup);

    PROCESS_INFORMATION process{};

    if (!CreateProcessW(
            executable.c_str(),
            mutableCommand.data(),
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            nullptr,
            &startup,
            &process))
    {
        throw std::runtime_error(
            "CreateProcessW failed with error " +
            std::to_string(
                GetLastError()));
    }

    CloseHandle(
        process.hThread);

    const DWORD wait =
        WaitForSingleObject(
            process.hProcess,
            60'000);

    if (wait != WAIT_OBJECT_0)
    {
        TerminateProcess(
            process.hProcess,
            0xFFFFFFFFU);
        CloseHandle(
            process.hProcess);

        throw std::runtime_error(
            "Child process did not finish within the test timeout.");
    }

    DWORD exitCode =
        0xFFFFFFFFU;

    if (!GetExitCodeProcess(
            process.hProcess,
            &exitCode))
    {
        CloseHandle(
            process.hProcess);

        throw std::runtime_error(
            "GetExitCodeProcess failed.");
    }

    CloseHandle(
        process.hProcess);

    return exitCode;
}

class TemporaryProject
{
public:
    TemporaryProject()
        : root_(
              std::filesystem::
                  temp_directory_path() /
              ("orbit-cli-package-" +
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

int main(
    const int argc,
    char** argv)
{
    try
    {
        ORBIT_TEST_CHECK(
            argc >= 2);

        const std::filesystem::path
            orbitBuild =
                std::filesystem::
                    weakly_canonical(
                        argv[1]);

        ORBIT_TEST_CHECK(
            std::filesystem::
                is_regular_file(
                    orbitBuild));

        TemporaryProject temporary;

        auto project =
            orbit::documents::
                ProjectDocument::Create(
                    temporary.Root(),
                    "CLI Package Test");

        WriteText(
            temporary.Root() /
                "Scripts/main.luau",
            "assert(Orbit.project_name == \"CLI Package Test\")\n"
            "return true\n");

        WriteTinyBmp(
            temporary.Root() /
                "Content/Test.bmp");

        WriteText(
            temporary.Root() /
                "Content/Test.orbitmaterial",
            "[material]\n"
            "name = \"CLI Material\"\n"
            "base_color = \"Test.bmp\"\n"
            "roughness_factor = 0.5\n"
            "metallic_factor = 0.0\n");


        WriteText(
            temporary.Root() /
                "Content/Shaders/Smoke.hlsl",
            "[numthreads(1, 1, 1)]\n"
            "void CSMain(uint3 id : SV_DispatchThreadID) {}\n");

        WriteText(
            temporary.Root() /
                "Content/Shaders/Smoke.hlsl.orbitshader.toml",
            "[shader]\n"
            "stage = \"compute\"\n"
            "entry = \"CSMain\"\n");

        project.Manifest().
            scriptEntryPoints = {
                "Scripts/main.luau"
            };
        project.Save();

        std::filesystem::remove_all(
            temporary.Root() /
                ".orbit" /
                "DerivedData");

        const std::vector<std::wstring>
            packageArguments{
                Quote(
                    temporary.Root()),
                L"--package",
                L"--profile",
                L"\"Development Windows\""
            };

        ORBIT_TEST_CHECK(
            RunProcess(
                orbitBuild,
                packageArguments) ==
            0U);

        const auto packageRoot =
            temporary.Root() /
            "Build" /
            "Development_Windows";

        const auto buildManifest =
            packageRoot /
            "OrbitBuildManifest.toml";

        const auto packageManifest =
            packageRoot /
            "OrbitPackage.toml";

        const auto gameExecutable =
            packageRoot /
            "CLI_Package_Test.exe";

        ORBIT_TEST_CHECK(
            std::filesystem::
                is_regular_file(
                    buildManifest));
        ORBIT_TEST_CHECK(
            std::filesystem::
                is_regular_file(
                    packageManifest));
        ORBIT_TEST_CHECK(
            std::filesystem::
                is_regular_file(
                    gameExecutable));

        const std::string firstManifest =
            ReadText(
                buildManifest);

        std::filesystem::remove_all(
            temporary.Root() /
                ".orbit" /
                "DerivedData");

        ORBIT_TEST_CHECK(
            RunProcess(
                orbitBuild,
                packageArguments) ==
            0U);

        ORBIT_TEST_CHECK(
            ReadText(
                buildManifest) ==
            firstManifest);

        ORBIT_TEST_CHECK(
            RunProcess(
                gameExecutable,
                {
                    Quote(
                        packageRoot),
                    L"--validate-only"
                }) ==
            0U);

        return 0;
    }
    catch (const std::exception&
               exception)
    {
        std::cerr
            << "OrbitBuild CLI test threw: "
            << exception.what()
            << '\n';
        return 1;
    }
}

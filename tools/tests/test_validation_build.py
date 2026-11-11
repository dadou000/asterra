"""Check validation isolation using the real module CMake files without a GPU SDK."""
import re
import shutil
import subprocess
import os
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def cmake_generator_args():
    """Reuse the configured toolchain when the repo already has a build tree."""
    if os.name == "nt" and shutil.which("ninja"):
        compiler = None
        vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / \
            "Microsoft Visual Studio/Installer/vswhere.exe"
        if vswhere.exists():
            install = subprocess.run(
                [str(vswhere), "-latest", "-property", "installationPath"],
                capture_output=True, text=True, check=False).stdout.strip()
            compiler_root = Path(install) / "VC/Tools/MSVC" if install else None
            if compiler_root and compiler_root.exists():
                compilers = sorted(compiler_root.glob("*/bin/Hostx64/x64/cl.exe"))
                if compilers:
                    compiler = compilers[-1].as_posix()
        if compiler:
            # This harness only inspects CMake targets. Ninja plus a forced
            # compiler avoids running a throwaway MSBuild compile probe.
            return ["-G", "Ninja", "-DCMAKE_SYSTEM_NAME=Generic",
                    f"-DCMAKE_CXX_COMPILER={compiler}",
                    "-DCMAKE_CXX_COMPILER_ID=MSVC",
                    "-DCMAKE_CXX_COMPILER_VERSION=19.44",
                    "-DCMAKE_CXX_COMPILER_FORCED=TRUE",
                    "-DCMAKE_CXX_COMPILER_WORKS=TRUE",
                    "-DCMAKE_CXX_COMPILE_FEATURES=cxx_std_23"]
    cache = ROOT / "build" / "CMakeCache.txt"
    if not cache.exists():
        return []
    values = {}
    for line in cache.read_text(errors="replace").splitlines():
        if line.startswith("CMAKE_GENERATOR:") or line.startswith("CMAKE_GENERATOR_INSTANCE:"):
            key, _, value = line.partition("=")
            values[key.split(":", 1)[0]] = value
    args = []
    generator = values.get("CMAKE_GENERATOR")
    instance = values.get("CMAKE_GENERATOR_INSTANCE")
    if generator:
        args.extend(["-G", generator])
    if instance:
        args.append(f"-DCMAKE_GENERATOR_INSTANCE={instance}")
    return args


@unittest.skipUnless(shutil.which("cmake"), "CMake is needed for target graph checks")
class ValidationBuildGraphTests(unittest.TestCase):
    def test_validation_isolation_matrix(self):
        cmake_files = [ROOT / "engine/studio_session/CMakeLists.txt",
                       ROOT / "engine/studio_ui/CMakeLists.txt",
                       ROOT / "tests/CMakeLists.txt"]
        # Only unrelated dependencies are stubbed. The production and support
        # libraries and test targets are created by their actual CMake files.
        aliases = set()
        for path in cmake_files:
            aliases.update(re.findall(r"Orbit::\w+", path.read_text()))
        aliases -= {"Orbit::StudioSession", "Orbit::StudioUi"}
        with tempfile.TemporaryDirectory(prefix="orbit-validation-graph-") as temp:
            directory = Path(temp)
            source = directory / "source"
            source.mkdir()
            (source / "stub.cpp").write_text("int main() { return 0; }\n")
            harness = """cmake_minimum_required(VERSION 3.28)
project(OrbitValidationGraph LANGUAGES CXX)
include(CTest)
function(orbit_enable_warnings target)
endfunction()
function(orbit_copy_dxc_runtime target)
endfunction()
add_library(SQLiteCpp INTERFACE)
add_executable(OrbitPlayer EXCLUDE_FROM_ALL stub.cpp)
add_custom_target(OrbitStudio)
"""
            for alias in sorted(aliases):
                harness += f"add_library({alias} INTERFACE IMPORTED)\n"
            # PROJECT_SOURCE_DIR must point at the real tooling source tree.
            harness += f'set(PROJECT_SOURCE_DIR "{ROOT.as_posix()}")\n'
            for index, path in enumerate(cmake_files):
                if index == 2:
                    harness += "if(BUILD_TESTING)\n"
                harness += (f'add_subdirectory("{path.parent.as_posix()}" '
                            f'"${{CMAKE_BINARY_DIR}}/module{index}")\n')
                if index == 2:
                    harness += "endif()\n"
            for target in ["OrbitStudioSession", "OrbitStudioUi",
                           "OrbitTerrainValidationSupport", "OrbitSceneValidationSupport"]:
                harness += f"""
if(TARGET {target})
    get_target_property(sources {target} SOURCES)
    get_target_property(links {target} LINK_LIBRARIES)
    get_target_property(includes {target} INTERFACE_INCLUDE_DIRECTORIES)
    file(WRITE "${{CMAKE_BINARY_DIR}}/{target}.txt" "${{sources}}\n${{links}}\n${{includes}}")
endif()
"""
            (source / "CMakeLists.txt").write_text(harness)
            for testing in [False, True]:
                for validation in [False, True]:
                    with self.subTest(testing=testing, validation=validation):
                        build = directory / f"build-{testing}-{validation}"
                        result = subprocess.run([
                            "cmake", "-S", str(source), "-B", str(build),
                            *cmake_generator_args(),
                            f"-DBUILD_TESTING={'ON' if testing else 'OFF'}",
                            f"-DORBIT_ENABLE_VALIDATION_TOOLS={'ON' if validation else 'OFF'}",
                        ], capture_output=True, text=True)
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                        session = (build / "OrbitStudioSession.txt").read_text()
                        ui = (build / "OrbitStudioUi.txt").read_text()
                        self.assertNotIn("StudioTerrainValidationScenario", session)
                        self.assertNotIn("OrbitTerrainValidationSupport", session)
                        if validation:
                            self.assertIn("SceneValidationScenarios.cpp", ui)
                            self.assertIn("OrbitTerrainValidationSupport", ui)
                        else:
                            self.assertNotIn("ValidationSupport", ui)
                            self.assertNotIn("SceneValidation", ui)
                            self.assertNotIn("tools/validation/include", ui)
                        self.assertEqual((build / "OrbitTerrainValidationSupport.txt").exists(),
                                         testing or validation)
                        self.assertEqual((build / "OrbitSceneValidationSupport.txt").exists(), testing)
                        if testing:
                            ctest = subprocess.run(["ctest", "--test-dir", str(build), "-N"],
                                                   capture_output=True, text=True)
                            self.assertIn("Orbit.TerrainCrossSystemRegression", ctest.stdout)
                            self.assertIn("Orbit.SceneDeterministicValidation", ctest.stdout)
                            self.assertNotIn("Orbit.V004IntegrationGate", ctest.stdout)


if __name__ == "__main__":
    unittest.main()

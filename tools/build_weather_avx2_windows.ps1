param(
    [string]$GodotCppDir = ""
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$source = Join-Path $repo "native\weather"
$build = Join-Path $source "build"
$bin = Join-Path $repo "bin"

Write-Host "Asterra AVX2 weather build"
Write-Host "  source: $source"
Write-Host "  output: $bin"

$cmakeArgs = @("-S", $source, "-B", $build)
if ($GodotCppDir -ne "") {
    $resolvedGodotCpp = (Resolve-Path $GodotCppDir).Path
    $cmakeArgs += "-DGODOT_CPP_DIR=$resolvedGodotCpp"
    Write-Host "  godot-cpp: $resolvedGodotCpp"
} else {
    Write-Host "  godot-cpp: FetchContent (master)"
}

& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }

& cmake --build $build --config Release --target asterra_weather --parallel
if ($LASTEXITCODE -ne 0) { throw "CMake build failed ($LASTEXITCODE)" }

$dll = Join-Path $bin "asterra_weather.dll"
$manifest = Join-Path $bin "asterra_weather.gdextension"
if (-not (Test-Path $dll)) { throw "Build completed but DLL was not found at $dll" }
if (-not (Test-Path $manifest)) { throw "Build completed but manifest was not found at $manifest" }

Write-Host ""
Write-Host "AVX2 weather backend built successfully."
Write-Host "  $dll"
Write-Host "  $manifest"
Write-Host "Restart Godot so WeatherNativeBootstrap can load the extension."

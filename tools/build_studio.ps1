<#
Inner-loop build: compile and link only OrbitStudio (Release) and republish the root Orbit.exe.

build_orbit.bat also re-runs the CMake configure, builds OrbitBuild and OrbitPlayer and packages dist/; none of that is needed to
see an engine change. Close Studio first (this script stops every Orbit* process, including a hot-generation child that would
otherwise keep the RPC port): editing source while Studio runs starts a hot handoff instead.

    powershell -File tools/build_studio.ps1 [-Config Release]

Prints the seconds taken and the first errors.
#>
param([string]$Config = "Release")

$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo
Get-Process Orbit* -ErrorAction SilentlyContinue | Stop-Process -Force

$stopwatch = [Diagnostics.Stopwatch]::StartNew()
$log = Join-Path $env:TEMP "orbit_build_studio.log"
cmake --build build --config $Config --target OrbitStudio --parallel *> $log
$code = $LASTEXITCODE
$stopwatch.Stop()

Select-String -Path $log -Pattern " error " | Select-Object -First 8 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(260, $_.Line.Length)) }
$exe = Get-Item (Join-Path $repo "Orbit.exe") -ErrorAction SilentlyContinue
"OrbitStudio $Config exit $code in $([int]$stopwatch.Elapsed.TotalSeconds) s; Orbit.exe $($exe.LastWriteTime)"
exit $code

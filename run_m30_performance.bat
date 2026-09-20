@echo off
setlocal EnableExtensions

cd /d "%~dp0"

set "CONFIG=Release"
set "NO_PAUSE=0"

:parse_args
if "%~1"=="" goto args_done

if /I "%~1"=="debug" (
    set "CONFIG=Debug"
    shift
    goto parse_args
)

if /I "%~1"=="release" (
    set "CONFIG=Release"
    shift
    goto parse_args
)

if /I "%~1"=="nopause" (
    set "NO_PAUSE=1"
    shift
    goto parse_args
)

echo [M30] Unknown option: %~1
echo Usage: run_m30_performance.bat [release^|debug] [nopause]
goto fail

:args_done

echo.
echo ============================================================
echo   Orbit V0.0.4 M30 performance capture
echo ============================================================
echo   Configuration: %CONFIG%
echo ============================================================
echo.

where cmake >nul 2>nul
if errorlevel 1 (
    echo [M30] ERROR: CMake was not found in PATH.
    goto fail
)

where python >nul 2>nul
if errorlevel 1 (
    echo [M30] ERROR: Python was not found in PATH.
    goto fail
)

echo [M30] Configuring Orbit...
cmake -S . -B build -DORBIT_WARNINGS_AS_ERRORS=ON
if errorlevel 1 (
    echo [M30] ERROR: CMake configure failed.
    goto fail
)

echo.
echo [M30] Building deterministic validation + performance diagnostic...
cmake --build build --config %CONFIG% --target OrbitV004ValidationTests OrbitV004TerrainPerformance OrbitWaterServiceTests --parallel
if errorlevel 1 (
    echo [M30] ERROR: M30 validation/performance build failed.
    goto fail
)

set "VALIDATION_EXE=build\tests\%CONFIG%\OrbitV004ValidationTests.exe"
set "PERF_EXE=build\tests\%CONFIG%\OrbitV004TerrainPerformance.exe"
set "WATER_EXE=build\engine\terrain_water\%CONFIG%\OrbitWaterServiceTests.exe"
set "RAW_CSV=build\m30-performance-%CONFIG%.csv"

if not exist "%VALIDATION_EXE%" (
    echo [M30] ERROR: Deterministic validation executable was not produced:
    echo   %VALIDATION_EXE%
    goto fail
)

if not exist "%PERF_EXE%" (
    echo [M30] ERROR: Diagnostic executable was not produced:
    echo   %PERF_EXE%
    goto fail
)

echo.
echo [M30] Running 20/20 deterministic validation...
"%VALIDATION_EXE%"
if errorlevel 1 (
    echo [M30] ERROR: Deterministic M30 validation failed.
    goto fail
)

echo.
echo [M30] Running WaterService normative extension acceptance...
"%WATER_EXE%"
if errorlevel 1 (
    echo [M30] ERROR: WaterService V0.0.4 acceptance failed.
    goto fail
)

echo.
echo [M30] Running GPU diagnostic...
"%PERF_EXE%" "%RAW_CSV%"
if errorlevel 1 (
    echo [M30] ERROR: GPU diagnostic failed.
    goto fail
)

if not exist "%RAW_CSV%" (
    echo [M30] ERROR: Diagnostic did not produce:
    echo   %RAW_CSV%
    goto fail
)

echo.
echo [M30] Validating captured record...
python tools\validate_v004_m30_performance.py "%RAW_CSV%"
if errorlevel 1 (
    echo [M30] ERROR: Performance record validation failed.
    goto fail
)

echo.
echo [M30] Importing validated capture and advancing the ledger...
python tools\import_v004_m30_performance.py "%RAW_CSV%" "%CONFIG%"
if errorlevel 1 (
    echo [M30] ERROR: Capture import failed.
    goto fail
)

echo.
echo [M30] Checking M31 entry gate...
python tools\check_v004_m31_gate.py
if errorlevel 1 (
    echo [M30] ERROR: M31 entry gate is still closed.
    goto fail
)

echo.
echo [M31] Enabling final integration CTest registration...
cmake -S . -B build -DORBIT_WARNINGS_AS_ERRORS=ON -DORBIT_ENABLE_V004_M31_GATE=ON
if errorlevel 1 (
    echo [M31] ERROR: Failed to enable the final integration gate.
    goto fail
)

echo.
echo [M31] Building final integration target...
cmake --build build --config %CONFIG% --target OrbitV004IntegrationGateTests --parallel
if errorlevel 1 (
    echo [M31] ERROR: Final integration target build failed.
    goto fail
)

set "M31_EXE=build\tests\%CONFIG%\OrbitV004IntegrationGateTests.exe"

if not exist "%M31_EXE%" (
    echo [M31] ERROR: Final integration executable was not produced:
    echo   %M31_EXE%
    goto fail
)

echo.
echo [M31] Executing aggregate V0.0.4 completion gate...
python tools\complete_v004_m31.py "%M31_EXE%" "%CONFIG%"
if errorlevel 1 (
    echo [M31] ERROR: Final integration gate failed.
    goto fail
)

echo.
echo ============================================================
echo [V0.0.4] COMPLETE
echo.
echo Canonical M30 performance record:
echo   docs\research\v004-m30-performance.csv
echo.
echo M30 summary:
echo   docs\V0.0.4_M30_PERFORMANCE_CAPTURE.md
echo.
echo M31 summary:
echo   docs\V0.0.4_M31_INTEGRATION_GATE.md
echo ============================================================
echo.

goto success

:fail
echo.
echo [M30] Capture did not complete.
if "%NO_PAUSE%"=="0" pause
exit /b 1

:success
if "%NO_PAUSE%"=="0" pause
exit /b 0

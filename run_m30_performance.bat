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
cmake --build build --config %CONFIG% --target OrbitV004ValidationTests OrbitV004TerrainPerformance --parallel
if errorlevel 1 (
    echo [M30] ERROR: M30 validation/performance build failed.
    goto fail
)

set "VALIDATION_EXE=build\tests\%CONFIG%\OrbitV004ValidationTests.exe"
set "PERF_EXE=build\tests\%CONFIG%\OrbitV004TerrainPerformance.exe"
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
echo ============================================================
echo [M30] CAPTURE SUCCESS
echo.
echo Canonical record:
echo   docs\research\v004-m30-performance.csv
echo.
echo Summary:
echo   docs\V0.0.4_M30_PERFORMANCE_CAPTURE.md
echo.
echo M31 entry gate: OPEN
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

@echo off
setlocal EnableExtensions

cd /d "%~dp0..\.."

set "CONFIG=Release"
set "NO_BUILD=0"
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

if /I "%~1"=="nobuild" (
    set "NO_BUILD=1"
    shift
    goto parse_args
)

if /I "%~1"=="nopause" (
    set "NO_PAUSE=1"
    shift
    goto parse_args
)

echo.
echo [Orbit] Unknown option: %~1
echo Usage: tools/validation/tools\validation\run_terrain_ui_smoke.bat [debug^|release] [nobuild] [nopause]
goto fail

:args_done

echo.
echo ============================================================
echo   Orbit Terrain UI real-device smoke
echo ============================================================
echo   Configuration : %CONFIG%
echo   Build first   : %NO_BUILD%
echo ============================================================
echo.

if "%NO_BUILD%"=="0" (
    where cmake >nul 2>nul
    if errorlevel 1 (
        echo [Orbit] ERROR: CMake was not found in PATH.
        goto fail
    )

    echo [Orbit] Configuring...
    cmake -S . -B build -DORBIT_WARNINGS_AS_ERRORS=ON
    if errorlevel 1 (
        echo [Orbit] ERROR: CMake configure failed.
        goto fail
    )

    echo [Orbit] Building and running the Studio smoke target...
    cmake --build build --config %CONFIG% --target OrbitStudioTerrainUiSmoke --parallel
    if errorlevel 1 (
        echo [Orbit] ERROR: Studio smoke target failed.
        goto fail
    )
)

if "%NO_BUILD%"=="0" goto passed

set "STUDIO=build\apps\editor\%CONFIG%\OrbitStudio.exe"
if not exist "%STUDIO%" set "STUDIO=Orbit.exe"
if not exist "%STUDIO%" (
    echo [Orbit] ERROR: Orbit Studio was not found. Build it first.
    goto fail
)
"%STUDIO%" --terrain-ui-smoke
set "SMOKE_EXIT=%ERRORLEVEL%"

if not "%SMOKE_EXIT%"=="0" (
    echo.
    echo [Orbit] TERRAIN UI SMOKE FAILED with exit code %SMOKE_EXIT%.
    goto fail
)

:passed
echo.
echo ============================================================
echo   Terrain UI smoke PASSED
echo ============================================================
echo   - active Surface Authoring panel registered
echo   - active Project Settings controls rendered
echo   - real Studio UI draw path exercised
echo   - production Vulkan terrain viewport rendered
echo   - save/reopen terrain round trip passed
echo ============================================================
echo.

if "%NO_PAUSE%"=="0" pause
exit /b 0

:fail
echo.
if "%NO_PAUSE%"=="0" pause
exit /b 1

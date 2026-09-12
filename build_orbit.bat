@echo off
setlocal EnableExtensions

cd /d "%~dp0"

set "CONFIG=Release"
set "DO_RUN=0"
set "DO_REBUILD=0"
set "NO_PAUSE=0"

:parse_args
if "%~1"=="" goto args_done

if /I "%~1"=="run" (
    set "DO_RUN=1"
    shift
    goto parse_args
)

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

if /I "%~1"=="rebuild" (
    set "DO_REBUILD=1"
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
echo.
echo Usage:
echo   build_orbit.bat
echo   build_orbit.bat run
echo   build_orbit.bat debug
echo   build_orbit.bat debug run
echo   build_orbit.bat rebuild run
echo   build_orbit.bat nopause
echo.
goto fail

:args_done

echo.
echo ============================================================
echo   Orbit local Windows builder
echo ============================================================
echo   Configuration : %CONFIG%
echo   Run after build: %DO_RUN%
echo   Full rebuild   : %DO_REBUILD%
echo ============================================================
echo.

where cmake >nul 2>nul
if errorlevel 1 (
    echo [Orbit] ERROR: CMake was not found in PATH.
    echo.
    echo Install CMake 3.28+ and Visual Studio 2022 with:
    echo   Desktop development with C++
    echo.
    goto fail
)

for /f "tokens=3" %%V in ('cmake --version ^| findstr /B /C:"cmake version"') do (
    echo [Orbit] CMake %%V
)

if "%DO_REBUILD%"=="1" (
    if exist "build" (
        echo [Orbit] Removing previous build directory...
        rmdir /s /q "build"

        if exist "build" (
            echo [Orbit] ERROR: Could not remove the build directory.
            goto fail
        )
    )
)

echo.
echo [Orbit] Configuring...
cmake -S . -B build -DORBIT_WARNINGS_AS_ERRORS=ON
if errorlevel 1 (
    echo.
    echo [Orbit] ERROR: CMake configure failed.
    echo Check that Visual Studio 2022 C++ tools are installed.
    goto fail
)

echo.
echo [Orbit] Building OrbitLauncher and OrbitSandbox...
cmake --build build --config %CONFIG% --target OrbitLauncher --parallel
if errorlevel 1 (
    echo.
    echo [Orbit] ERROR: Build failed.
    goto fail
)

set "LAUNCHER=build\apps\launcher\%CONFIG%\OrbitLauncher.exe"
set "SANDBOX=build\apps\sandbox\%CONFIG%\OrbitSandbox.exe"

if not exist "%LAUNCHER%" (
    echo.
    echo [Orbit] ERROR: Launcher was not produced:
    echo   %LAUNCHER%
    goto fail
)

if not exist "%SANDBOX%" (
    echo.
    echo [Orbit] ERROR: Sandbox was not produced:
    echo   %SANDBOX%
    goto fail
)

set "PACKAGE=dist\Orbit-Windows-%CONFIG%"
set "SYMBOLS=%PACKAGE%\symbols"

echo.
echo [Orbit] Packaging...

if exist "%PACKAGE%" (
    rmdir /s /q "%PACKAGE%"
)

mkdir "%PACKAGE%" >nul 2>nul
mkdir "%SYMBOLS%" >nul 2>nul

copy /y "%LAUNCHER%" "%PACKAGE%\OrbitLauncher.exe" >nul
if errorlevel 1 goto package_fail

copy /y "%SANDBOX%" "%PACKAGE%\OrbitSandbox.exe" >nul
if errorlevel 1 goto package_fail

if exist "build\apps\launcher\%CONFIG%\OrbitLauncher.pdb" (
    copy /y "build\apps\launcher\%CONFIG%\OrbitLauncher.pdb" "%SYMBOLS%\OrbitLauncher.pdb" >nul
)

if exist "build\apps\sandbox\%CONFIG%\OrbitSandbox.pdb" (
    copy /y "build\apps\sandbox\%CONFIG%\OrbitSandbox.pdb" "%SYMBOLS%\OrbitSandbox.pdb" >nul
)

> "%PACKAGE%\README.txt" (
    echo Orbit Windows %CONFIG%
    echo =====================
    echo.
    echo Start Orbit with:
    echo     OrbitLauncher.exe
    echo.
    echo Runtime logs and crash reports are written under:
    echo     logs\
    echo.
    echo Keep symbols\ when diagnosing .dmp crash files.
)

echo.
echo ============================================================
echo [Orbit] BUILD SUCCESS
echo.
echo Launcher:
echo   %CD%\%PACKAGE%\OrbitLauncher.exe
echo.
echo Runtime:
echo   %CD%\%PACKAGE%\OrbitSandbox.exe
echo ============================================================
echo.

if "%DO_RUN%"=="1" (
    echo [Orbit] Starting launcher...
    start "" "%PACKAGE%\OrbitLauncher.exe"
)

goto success

:package_fail
echo.
echo [Orbit] ERROR: Failed to package the executables.
goto fail

:fail
echo.
echo [Orbit] Build did not complete.
if "%NO_PAUSE%"=="0" pause
exit /b 1

:success
if "%NO_PAUSE%"=="0" pause
exit /b 0

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

set "GIT_SHA=nogit"
where git >nul 2>nul
if not errorlevel 1 (
    for /f %%G in ('git rev-parse --short^=8 HEAD 2^>nul') do set "GIT_SHA=%%G"
)
echo [Orbit] Source commit %GIT_SHA%

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
echo [Orbit] Building OrbitStudio, OrbitBuild and OrbitPlayer...
cmake --build build --config %CONFIG% --target OrbitStudio OrbitBuild OrbitPlayer --parallel
if errorlevel 1 (
    echo.
    echo [Orbit] ERROR: Build failed.
    goto fail
)

set "STUDIO=build\apps\editor\%CONFIG%\OrbitStudio.exe"
set "BUILDCLI=build\apps\build\%CONFIG%\OrbitBuild.exe"
set "PLAYER=build\apps\player\%CONFIG%\OrbitPlayer.exe"
set "DXC=build\apps\build\%CONFIG%\dxcompiler.dll"

if not exist "%STUDIO%" (
    echo.
    echo [Orbit] ERROR: Studio was not produced:
    echo   %STUDIO%
    goto fail
)

if not exist "%BUILDCLI%" (
    echo.
    echo [Orbit] ERROR: OrbitBuild was not produced:
    echo   %BUILDCLI%
    goto fail
)

if not exist "%PLAYER%" (
    echo.
    echo [Orbit] ERROR: OrbitPlayer was not produced:
    echo   %PLAYER%
    goto fail
)

if not exist "%DXC%" (
    echo.
    echo [Orbit] ERROR: dxcompiler.dll was not deployed:
    echo   %DXC%
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

copy /y "%STUDIO%" "%PACKAGE%\OrbitStudio.exe" >nul
if errorlevel 1 goto package_fail

copy /y "%BUILDCLI%" "%PACKAGE%\OrbitBuild.exe" >nul
if errorlevel 1 goto package_fail

copy /y "%PLAYER%" "%PACKAGE%\OrbitPlayer.exe" >nul
if errorlevel 1 goto package_fail

copy /y "%DXC%" "%PACKAGE%\dxcompiler.dll" >nul
if errorlevel 1 goto package_fail

echo [Orbit] Updating root executables...
copy /y "%STUDIO%" "Orbit.exe" >nul
if errorlevel 1 goto root_copy_fail
copy /y "%PLAYER%" "OrbitPlayer.exe" >nul
if errorlevel 1 goto root_copy_fail
copy /y "%DXC%" "dxcompiler.dll" >nul
if errorlevel 1 goto root_copy_fail

if exist "build\apps\editor\%CONFIG%\OrbitStudio.pdb" (
    copy /y "build\apps\editor\%CONFIG%\OrbitStudio.pdb" "%SYMBOLS%\OrbitStudio.pdb" >nul
)

if exist "build\apps\build\%CONFIG%\OrbitBuild.pdb" (
    copy /y "build\apps\build\%CONFIG%\OrbitBuild.pdb" "%SYMBOLS%\OrbitBuild.pdb" >nul
)

if exist "build\apps\player\%CONFIG%\OrbitPlayer.pdb" (
    copy /y "build\apps\player\%CONFIG%\OrbitPlayer.pdb" "%SYMBOLS%\OrbitPlayer.pdb" >nul
)

> "%PACKAGE%\README.txt" (
    echo Orbit Windows %CONFIG%
    echo =====================
    echo.
    echo Start Orbit Studio with:
    echo     OrbitStudio.exe [project-directory-or-Project.orbit.toml]
    echo.
    echo The local one-click build also publishes the same Studio executable as:
    echo     ^<repo-root^>\Orbit.exe
    echo.
    echo Validate or cook a project headlessly with:
    echo     OrbitBuild.exe ^<project-directory-or-Project.orbit.toml^> --validate
    echo     OrbitBuild.exe ^<project-directory-or-Project.orbit.toml^> --cook
    echo.
    echo Assemble a standalone project package with:
    echo     OrbitBuild.exe ^<project-directory-or-Project.orbit.toml^> --package
    echo.
    echo Run a cooked project directly with:
    echo     OrbitPlayer.exe ^<cooked-package-or-OrbitBuildManifest.toml^>
    echo.
    echo Runtime logs and crash reports are written under:
    echo     logs\
    echo.
    echo Keep symbols\ when diagnosing .dmp crash files.
)

echo.
echo ============================================================
echo [Orbit] BUILD SUCCESS  %GIT_SHA%
echo.
echo Root executable:
echo   %CD%\Orbit.exe
echo.
echo Runtime dependency:
echo   %CD%\OrbitPlayer.exe
echo.
echo Package:
echo   %CD%\%PACKAGE%\OrbitStudio.exe
echo ============================================================
echo.

if "%DO_RUN%"=="1" (
    echo [Orbit] Starting root executable %GIT_SHA%...
    start "" "%CD%\Orbit.exe"
)

goto success

:root_copy_fail
echo.
echo [Orbit] ERROR: Failed to update the root executables.
echo Close any running Orbit.exe / OrbitPlayer.exe and retry.
goto fail

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

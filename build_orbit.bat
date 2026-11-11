@echo off
setlocal EnableExtensions
cd /d "%~dp0"

rem Development is quick/unpackaged by default. Packaging is an explicit release step.
set "CONFIG=Release"
set "MODE=quick"
set "DO_RUN=0"
set "DO_REBUILD=0"
set "DO_RECONFIGURE=0"
set "NO_PAUSE=0"
set "BUILD_JOBS=2"
if defined ORBIT_BUILD_JOBS set "BUILD_JOBS=%ORBIT_BUILD_JOBS%"

:parse_args
if "%~1"=="" goto args_done
if /I "%~1"=="quick" (
    set "MODE=quick"
    shift
    goto parse_args
)
if /I "%~1"=="fast" (
    set "MODE=quick"
    shift
    goto parse_args
)
if /I "%~1"=="unpacked" (
    set "MODE=unpacked"
    shift
    goto parse_args
)
if /I "%~1"=="package" (
    set "MODE=package"
    shift
    goto parse_args
)
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
if /I "%~1"=="reconfigure" (
    set "DO_RECONFIGURE=1"
    shift
    goto parse_args
)
if /I "%~1"=="nopause" (
    set "NO_PAUSE=1"
    shift
    goto parse_args
)
if /I "%~1"=="jobs" (
    if "%~2"=="" goto bad_jobs
    set "BUILD_JOBS=%~2"
    shift
    shift
    goto parse_args
)
echo [Orbit] Unknown option: %~1
goto usage

:bad_jobs
echo [Orbit] Expected a positive integer after "jobs".
goto usage

:usage
echo.
echo Usage:
echo   build_orbit.bat                          ^(fast incremental Studio only^)
echo   build_orbit.bat unpacked run             ^(launch from an isolated dev generation^)
echo   build_orbit.bat debug unpacked run
echo   build_orbit.bat package                  ^(build Studio/Player/Build and package^)
echo   build_orbit.bat package run
echo   build_orbit.bat reconfigure
echo   build_orbit.bat rebuild package
echo   build_orbit.bat jobs 2 unpacked nopause
echo.
goto fail

:args_done
echo(%BUILD_JOBS%| findstr /R /C:"^[1-9][0-9]*$" >nul
if errorlevel 1 goto bad_jobs

echo.
echo ============================================================
echo   Orbit Windows development build
echo ============================================================
echo   Configuration: %CONFIG%
echo   Mode         : %MODE%
echo   Start Studio : %DO_RUN%
echo   Build workers: %BUILD_JOBS% MSBuild projects ^(MSVC /MP enabled^)
echo ============================================================
echo.

where cmake >nul 2>nul
if errorlevel 1 (
    echo [Orbit] ERROR: CMake missing. Install CMake and VS 2022 C++ tools.
    goto fail
)

set "GIT_SHA=nogit"
where git >nul 2>nul
if not errorlevel 1 (
    for /f %%G in ('git rev-parse --short^=8 HEAD 2^>nul') do set "GIT_SHA=%%G"
)
echo [Orbit] Source commit %GIT_SHA%

if "%DO_REBUILD%"=="1" (
    if exist "build" (
        echo [Orbit] Removing previous build for requested full rebuild...
        rmdir /s /q "build"
        if exist "build" (
            echo [Orbit] ERROR: Could not remove build directory.
            goto fail
        )
    )
)

rem The generated build system already notices and reconfigures when CMake
rem inputs change. Avoid paying the CMake configure cost on every iteration.
set "NEED_CONFIGURE=0"
if "%DO_RECONFIGURE%"=="1" set "NEED_CONFIGURE=1"
if not exist "build\CMakeCache.txt" set "NEED_CONFIGURE=1"
if "%NEED_CONFIGURE%"=="0" (
    findstr /C:"ORBIT_FAST_HOT_ITERATION:BOOL=ON" "build\CMakeCache.txt" >nul
    if errorlevel 1 set "NEED_CONFIGURE=1"
    findstr /C:"ORBIT_WARNINGS_AS_ERRORS:BOOL=ON" "build\CMakeCache.txt" >nul
    if errorlevel 1 set "NEED_CONFIGURE=1"
)

if "%NEED_CONFIGURE%"=="1" (
    echo [Orbit] Configuring development build...
    cmake -S . -B build -DORBIT_WARNINGS_AS_ERRORS=ON -DORBIT_FAST_HOT_ITERATION=ON
    if errorlevel 1 (
        echo [Orbit] ERROR: CMake configure failed.
        goto fail
    )
) else (
    echo [Orbit] Reusing CMakeCache.txt ^(CMake tracks build-script changes^).
)

if /I "%MODE%"=="package" goto build_package
echo [Orbit] Incremental Studio build ^(Player and hot-reload dependencies are automatic^)...
cmake --build build --config %CONFIG% --target OrbitStudio --parallel %BUILD_JOBS%
if errorlevel 1 goto build_fail
goto verify

:build_package
echo [Orbit] Full application build for explicit packaging...
cmake --build build --config %CONFIG% --target OrbitStudio OrbitBuild OrbitPlayer --parallel %BUILD_JOBS%
if errorlevel 1 goto build_fail

:verify
set "STUDIO=build\apps\editor\%CONFIG%\OrbitStudio.exe"
set "PLAYER=build\apps\player\%CONFIG%\OrbitPlayer.exe"
set "BUILDCLI=build\apps\build\%CONFIG%\OrbitBuild.exe"
set "DXC=build\apps\editor\%CONFIG%\dxcompiler.dll"
if not exist "%STUDIO%" (
    echo [Orbit] ERROR: Studio executable was not produced: %STUDIO%
    goto fail
)
if not exist "%DXC%" (
    echo [Orbit] ERROR: The Studio DXC runtime is missing: %DXC%
    goto fail
)
if not exist "%PLAYER%" (
    echo [Orbit] ERROR: Required Player target was not produced: %PLAYER%
    goto fail
)
if /I "%MODE%"=="package" (
    if not exist "%BUILDCLI%" (
        echo [Orbit] ERROR: Build CLI was not produced: %BUILDCLI%
        goto fail
    )
    goto package
)
goto built

:package
set "PACKAGE=dist\Orbit-Windows-%CONFIG%"
set "SYMBOLS=%PACKAGE%\symbols"
echo [Orbit] Assembling requested distribution...
if exist "%PACKAGE%" rmdir /s /q "%PACKAGE%"
if exist "%PACKAGE%" (
    echo [Orbit] ERROR: Could not replace package; close applications using it.
    goto fail
)
mkdir "%SYMBOLS%" >nul 2>nul
if errorlevel 1 goto package_fail
copy /y "%STUDIO%" "%PACKAGE%\OrbitStudio.exe" >nul
if errorlevel 1 goto package_fail
copy /y "%PLAYER%" "%PACKAGE%\OrbitPlayer.exe" >nul
if errorlevel 1 goto package_fail
copy /y "%BUILDCLI%" "%PACKAGE%\OrbitBuild.exe" >nul
if errorlevel 1 goto package_fail
copy /y "%DXC%" "%PACKAGE%\dxcompiler.dll" >nul
if errorlevel 1 goto package_fail
if exist "build\apps\editor\%CONFIG%\OrbitStudio.pdb" copy /y "build\apps\editor\%CONFIG%\OrbitStudio.pdb" "%SYMBOLS%\OrbitStudio.pdb" >nul
if exist "build\apps\build\%CONFIG%\OrbitBuild.pdb" copy /y "build\apps\build\%CONFIG%\OrbitBuild.pdb" "%SYMBOLS%\OrbitBuild.pdb" >nul
if exist "build\apps\player\%CONFIG%\OrbitPlayer.pdb" copy /y "build\apps\player\%CONFIG%\OrbitPlayer.pdb" "%SYMBOLS%\OrbitPlayer.pdb" >nul
> "%PACKAGE%\README.txt" (
    echo Orbit Windows %CONFIG%
    echo Studio: OrbitStudio.exe [project-directory-or-Project.orbit.toml]
    echo Player: OrbitPlayer.exe [cooked-project]
    echo Build: OrbitBuild.exe [project] --validate / --cook / --package
    echo This folder is a distribution. Use build_orbit.bat unpacked run for iteration.
)
echo [Orbit] Distribution: %CD%\%PACKAGE%

:built
echo.
echo [Orbit] BUILD SUCCESS %GIT_SHA%
echo [Orbit] Development Studio: %CD%\%STUDIO%
echo [Orbit] CMake publishes root Orbit.exe when not locked by a running generation.
if "%DO_RUN%"=="0" goto success

rem Never launch the output that incremental LINK needs to replace. Stage
rem only the same Studio executable and its adjacent runtime dependencies.
rem This is not a separate launcher or a full distribution package.
set "STAGE=out\unpacked\%CONFIG%\%RANDOM%-%RANDOM%"
mkdir "%STAGE%" >nul 2>nul
if errorlevel 1 goto stage_fail
copy /y "%STUDIO%" "%STAGE%\Orbit.exe" >nul
if errorlevel 1 goto stage_fail
copy /y "%DXC%" "%STAGE%\dxcompiler.dll" >nul
if errorlevel 1 goto stage_fail
copy /y "%PLAYER%" "%STAGE%\OrbitPlayer.exe" >nul
if errorlevel 1 goto stage_fail
echo [Orbit] Starting unpackaged Studio generation %GIT_SHA% ...
start "" /D "%CD%" "%CD%\%STAGE%\Orbit.exe"
if errorlevel 1 goto stage_fail
goto success

:build_fail
echo [Orbit] ERROR: Native build failed. Existing running generation was not replaced.
goto fail

:package_fail
echo [Orbit] ERROR: Failed to assemble distribution.
goto fail

:stage_fail
echo [Orbit] ERROR: Failed to stage or start the unpackaged Studio.
goto fail

:fail
echo [Orbit] Build did not complete.
if "%NO_PAUSE%"=="0" pause
exit /b 1

:success
if "%NO_PAUSE%"=="0" pause
exit /b 0

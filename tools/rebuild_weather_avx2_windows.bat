@echo off
setlocal EnableExtensions

rem Asterra native weather one-click rebuild.
rem Double-click this file from Explorer or run it from any working directory.

cd /d "%~dp0.."

echo ============================================================
echo   ASTERRA - Rebuild Weather AVX2 Backend

echo   Repository: %CD%
echo ============================================================
echo.

rem Godot keeps the non-reloadable GDExtension DLL loaded. Refuse to build
rem while an editor/game process is running instead of killing unrelated work.
tasklist /FI "IMAGENAME eq godot*.exe" 2>NUL | find /I "godot" >NUL
if not errorlevel 1 (
    echo [ERROR] A Godot process is currently running.
    echo.
    echo Close the Godot editor and any running Asterra instance, then run
    echo this file again. The weather GDExtension is not hot-reloadable.
    echo.
    pause
    exit /b 2
)

if not exist "tools\build_weather_avx2_windows.ps1" (
    echo [ERROR] tools\build_weather_avx2_windows.ps1 was not found.
    echo Run this BAT from an Asterra checkout.
    echo.
    pause
    exit /b 3
)

echo [1/2] Building 1024x512x30 AVX2/OpenMP weather backend...
echo.

powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%CD%\tools\build_weather_avx2_windows.ps1"
set "BUILD_RESULT=%ERRORLEVEL%"

if not "%BUILD_RESULT%"=="0" (
    echo.
    echo ============================================================
    echo   BUILD FAILED - exit code %BUILD_RESULT%
    echo ============================================================
    echo.
    echo Review the compiler output above. The old DLL, if any, was not
    echo considered a successful rebuild.
    echo.
    pause
    exit /b %BUILD_RESULT%
)

echo.
echo [2/2] Verifying runtime files...
if not exist "bin\asterra_weather.dll" goto :missing_output
if not exist "bin\asterra_weather.gdextension" goto :missing_output

echo.
echo ============================================================
echo   WEATHER BACKEND REBUILT SUCCESSFULLY

echo   bin\asterra_weather.dll
    echo   bin\asterra_weather.gdextension

echo ============================================================
echo.
echo You can now start Godot. A full editor restart is required after every
 echo native weather rebuild.
echo.
pause
exit /b 0

:missing_output
echo.
echo [ERROR] The build command returned success, but one or more runtime
 echo files are missing from bin\.
echo.
pause
exit /b 4

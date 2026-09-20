@echo off
rem ============================================================================
rem msvc_ci.bat - MSVC environment wrapper with Visual Studio auto-discovery.
rem
rem CI-oriented sibling of msvc_build.bat (which hardcodes a local install
rem path). Locates the newest Visual Studio with the C++ toolset via
rem vswhere (preinstalled on GitHub windows runners and with VS Installer),
rem initializes vcvars64, prepends the VS-bundled Ninja directory to PATH
rem when present, then executes the given command line.
rem
rem Usage: msvc_ci.bat <command...>   e.g. msvc_ci.bat cmake -S . -B build
rem Exit codes: the wrapped command's exit code; 2 = environment discovery
rem failure.
rem ============================================================================
setlocal

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [msvc_ci] vswhere.exe not found at "%VSWHERE%"
    exit /b 2
)

set "VS_PATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%i"
if not defined VS_PATH (
    echo [msvc_ci] no Visual Studio installation with the VC toolset found
    exit /b 2
)

call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo [msvc_ci] vcvars64.bat failed under "%VS_PATH%"
    exit /b 2
)

if exist "%VS_PATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" (
    set "PATH=%VS_PATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
)

%*
exit /b %errorlevel%

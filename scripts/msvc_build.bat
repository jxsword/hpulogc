@echo off
rem MSVC build wrapper: usage: msvc_build.bat <command...>
rem Sets up the VS2026 dev environment then executes the given command.
call D:\VS2026\VC\Auxiliary\Build\vcvars64.bat >nul 2>&1
if errorlevel 1 (
    echo [msvc_build] vcvars64.bat failed
    exit /b 1
)
set "PATH=D:\VS2026\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
%*

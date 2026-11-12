@echo off
REM Builds FastChat.exe (Release) into this folder.
REM   build.bat          FastChat
REM   build.bat tests    FastChat and the core tests (build\Release\fastchat_tests.exe)
REM   build.bat no-prism FastChat without Prism (SAPI speech only)

setlocal enabledelayedexpansion
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Error: Visual Studio 2022 with C++ is required.
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSINSTALL=%%i"
if not exist "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" (
    echo Error: cannot find the Visual Studio C++ tools.
    exit /b 1
)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

set "PRISM=ON"
set "TARGETS=--target FastChat"
:args
if "%~1"=="" goto :done_args
if /i "%~1"=="no-prism" set "PRISM=OFF"
if /i "%~1"=="tests" set "TARGETS=--target FastChat fastchat_tests"
shift
goto :args
:done_args

REM The first run fetches and builds wxWidgets, Prism and the rest into build\,
REM which takes a while; later runs reuse them.
cmake -S . -B build -A x64 -DFASTCHAT_PRISM=%PRISM%
if errorlevel 1 goto :error
cmake --build build --config Release --parallel %TARGETS% -- /nodeReuse:false
if errorlevel 1 goto :error

echo.
echo Build successful. Run FastChat.exe to start.
exit /b 0

:error
echo.
echo Build failed.
exit /b 1

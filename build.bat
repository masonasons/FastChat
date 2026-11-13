@echo off
REM Builds FastChat.exe (Release) into this folder.
REM   build.bat          FastChat
REM   build.bat tests    FastChat and the core tests (build\Release\fastchat_tests.exe)
REM   build.bat no-prism FastChat without Prism (SAPI speech only)
REM   build.bat dist     FastChat, then FastChat.zip (dist.bat)

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
REM The CMake on the PATH (Prism needs CMake 4), found before vcvars64 puts
REM Visual Studio's own, older one ahead of it.
set "CMAKE="
for /f "delims=" %%c in ('where cmake 2^>nul') do if not defined CMAKE set "CMAKE=%%c"
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if not defined CMAKE set "CMAKE=cmake"

set "PRISM=ON"
set "DIST="
set "TARGETS=--target FastChat"
:args
if "%~1"=="" goto :done_args
if /i "%~1"=="no-prism" set "PRISM=OFF"
if /i "%~1"=="tests" set "TARGETS=--target FastChat fastchat_tests"
if /i "%~1"=="dist" set "DIST=1"
shift
goto :args
:done_args

REM The first run fetches and builds wxWidgets, Prism and the rest into build\,
REM which takes a while; later runs reuse them.
"%CMAKE%" -S . -B build -A x64 -DFASTCHAT_PRISM=%PRISM%
if errorlevel 1 goto :error
REM A running FastChat.exe cannot be replaced but can be renamed: move it aside so
REM the build can write the new one (restart FastChat to use it).
if exist FastChat.old.exe del /q FastChat.old.exe >nul 2>&1
if exist FastChat.exe move /y FastChat.exe FastChat.old.exe >nul 2>&1
"%CMAKE%" --build build --config Release --parallel %TARGETS% -- /nodeReuse:false
if errorlevel 1 goto :error

if defined DIST (
    call "%~dp0dist.bat"
    if errorlevel 1 goto :error
)

echo.
echo Build successful. Run FastChat.exe to start.
exit /b 0

:error
echo.
echo Build failed.
exit /b 1

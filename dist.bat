@echo off
REM Packages FastChat for others: FastChat.zip with FastChat.exe and the readme.
REM   dist.bat           FastChat.zip, which keeps its data in %APPDATA%\FastChat
REM   dist.bat portable  FastChat-portable.zip, which keeps its data beside the exe
REM
REM FastChat.exe needs nothing else: wxWidgets, Prism and the C runtime are
REM linked into it, and everything it loads comes with Windows.

setlocal
cd /d "%~dp0"

if not exist "FastChat.exe" (
    echo Error: FastChat.exe not found. Run build.bat first.
    exit /b 1
)

set "ZIPNAME=FastChat.zip"
set "PORTABLE="
if /i "%~1"=="portable" (
    set "ZIPNAME=FastChat-portable.zip"
    set "PORTABLE=1"
)

if exist "%ZIPNAME%" del "%ZIPNAME%"
if exist "dist_temp" rmdir /s /q "dist_temp"
mkdir "dist_temp\FastChat"

copy /y "FastChat.exe" "dist_temp\FastChat\" >nul
copy /y "README.md" "dist_temp\FastChat\" >nul
REM The 32-bit helper that speaks with 32-bit SAPI voices.
if exist "FastChatSapi32.exe" copy /y "FastChatSapi32.exe" "dist_temp\FastChat\" >nul
REM Sound packs that come with FastChat, if there are any.
if exist "soundpacks\" xcopy /e /i /q /y "soundpacks" "dist_temp\FastChat\soundpacks" >nul
REM portable.txt beside the exe keeps settings and conversations in that folder.
if defined PORTABLE echo FastChat keeps its settings and conversations in this folder.> "dist_temp\FastChat\portable.txt"

echo Creating %ZIPNAME%...
powershell -NoProfile -Command "Compress-Archive -Path 'dist_temp\FastChat' -DestinationPath '%ZIPNAME%' -Force"
if errorlevel 1 (
    rmdir /s /q "dist_temp"
    echo Failed to create %ZIPNAME%.
    exit /b 1
)
rmdir /s /q "dist_temp"

echo.
echo Created %ZIPNAME%:
powershell -NoProfile -Command "Add-Type -AssemblyName System.IO.Compression.FileSystem; $z = [System.IO.Compression.ZipFile]::OpenRead('%ZIPNAME%'); $z.Entries | ForEach-Object { Write-Host ('  ' + $_.FullName + ' (' + [math]::Round($_.Length/1KB) + ' KB)') }; $z.Dispose()"

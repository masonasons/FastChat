# Checks that FastChat.exe asks Windows only for what Windows 7 has: every DLL
# and function it imports, against Windows 7's own export lists (shipped with
# YY-Thunks in build\_deps). Run from the repository after build.bat:
#   powershell -ExecutionPolicy Bypass -File tests\check_win7.ps1 [path\to\exe]
param([string]$Exe = (Join-Path $PSScriptRoot "..\FastChat.exe"))
$ErrorActionPreference = "Stop"

$cfg = Join-Path $PSScriptRoot "..\build\_deps\yythunks-src\Config\x64\6.1.7600.txt"
if (-not (Test-Path $cfg)) { throw "Windows 7 export lists not found ($cfg); build with FASTCHAT_WIN7 on first." }
$win7 = @{}
$dll = $null
foreach ($line in [IO.File]::ReadLines((Resolve-Path $cfg))) {
    if ($line -match '^\[(.+)\]$') {
        $dll = $Matches[1].ToLower()
        $win7[$dll] = New-Object 'System.Collections.Generic.HashSet[string]'
    } elseif ($dll -and $line -match '^\d+=(.+)$') {
        [void]$win7[$dll].Add($Matches[1])
    }
}

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$imports = cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1 && dumpbin /nologo /imports `"$(Resolve-Path $Exe)`""
$problems = @()
$functions = 0
$dll = $null
foreach ($line in $imports) {
    if ($line -match '^\s{4}(\S+\.(dll|drv))$') {
        $dll = $Matches[1].ToLower()
        if (-not $win7.ContainsKey($dll)) { $problems += "$dll (no such DLL on Windows 7)" }
    } elseif ($dll -and $line -match '^\s+[0-9A-F]+\s+([A-Za-z_]\w*)$') {
        $functions++
        if ($win7.ContainsKey($dll) -and -not $win7[$dll].Contains($Matches[1])) { $problems += "$dll!$($Matches[1])" }
    }
}
if ($problems) {
    "Not on Windows 7:"
    $problems | ForEach-Object { "  $_" }
    exit 1
}
"$(Split-Path $Exe -Leaf): all $functions imported functions exist on Windows 7."

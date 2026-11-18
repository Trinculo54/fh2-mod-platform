#DISCLAIMER:
#This is a translation of "build.sh" to PowerShell. It is meant to be used by Windows users only and has not been tested on Linux.

<#
.SYNOPSIS
    Windows/PowerShell version of scripts/build.sh for the FH2 Mod Platform.

.DESCRIPTION
    Cross-compiles the platform with llvm-mingw + Ninja, stages dist\ the same way
    build.sh does, and can optionally install the result into your Azimuth folder.

    Put this file in the repo's scripts\ folder (next to build.sh) and run it from
    anywhere. It works out the repo root as the parent of the folder it sits in.

.PARAMETER Clean
    Delete the build\ folder first (use this if configure fails or you changed toolchains).

.PARAMETER InstallTo
    Path to the folder containing azimuth.exe. If given, the platform DLLs are copied
    there and an fh2-mods folder is created.

.PARAMETER IncludeExample
    With -InstallTo, also copy HelloMod.dll into fh2-mods (handy for a first test).

.EXAMPLE
    .\scripts\build.ps1

.EXAMPLE
    .\scripts\build.ps1 -Clean -InstallTo "C:\Games\Azimuth" -IncludeExample
#>
[CmdletBinding()]
param(
    [switch]$Clean,
    [string]$InstallTo,
    [switch]$IncludeExample
)

$ErrorActionPreference = 'Stop'

function Fail([string]$msg) {
    Write-Host "ERROR: $msg" -ForegroundColor Red
    exit 1
}

# Run a native command and stop if it returns a non-zero exit code.
function Invoke-Native([string]$exe, [string[]]$arguments) {
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { Fail "'$exe' failed with exit code $LASTEXITCODE." }
}

# ---------------------------------------------------------------- locate repo
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not (Test-Path (Join-Path $root 'CMakeLists.txt'))) {
    Fail "CMakeLists.txt not found in '$root'. Put this script in the repo's scripts\ folder."
}
$toolchain = Join-Path $root 'cmake\mingw-w64-x86_64.cmake'
if (-not (Test-Path $toolchain)) { Fail "Toolchain file not found: $toolchain" }
$toolchainArg = $toolchain -replace '\\', '/'
$build = Join-Path $root 'build'
$dist  = Join-Path $root 'dist'

Write-Host "Repo root: $root"

# ------------------------------------------------------------- check the tools
$missing = @()
foreach ($tool in 'cmake', 'ninja', 'x86_64-w64-mingw32-clang', 'x86_64-w64-mingw32-clang++') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { $missing += $tool }
}
if (-not (Get-Command 'x86_64-w64-mingw32-windres' -ErrorAction SilentlyContinue) -and
    -not (Get-Command 'llvm-windres' -ErrorAction SilentlyContinue)) {
    $missing += 'llvm-windres'
}
if ($missing.Count -gt 0) {
    Write-Host "These tools were not found on PATH: $($missing -join ', ')" -ForegroundColor Red
    Write-Host @"

Fixes:
  - cmake / ninja: install them and reopen this terminal (winget install Kitware.CMake Ninja-build.Ninja).
  - llvm-mingw: extract it, add its bin\ folder (the one containing clang.exe) to PATH,
    then reopen this terminal. Must be llvm-mingw, not MSYS2 or another MinGW.
"@
    exit 1
}

# -------------------------------------------------------------------- configure
if ($Clean -and (Test-Path $build)) {
    Write-Host "Removing $build"
    Remove-Item -Recurse -Force $build
}

Write-Host "`n== Configuring ==" -ForegroundColor Cyan
Invoke-Native 'cmake' @(
    '-S', $root,
    '-B', $build,
    '-G', 'Ninja',
    "-DCMAKE_TOOLCHAIN_FILE=$toolchainArg",
    '-DCMAKE_BUILD_TYPE=Release',
    '-DFH2MOD_PLATFORM_BUILD='   # empty on purpose: the build is named by its version
)

# ------------------------------------------------------------------------ build
Write-Host "`n== Building ==" -ForegroundColor Cyan
Invoke-Native 'cmake' @('--build', $build, '-j', [string][Environment]::ProcessorCount)

# ----------------------------------------------------------- check build output
$expected = 'd3dcompiler_47.dll', 'DWrite.dll', 'HelloMod.dll', 'fh2mod.exe'
foreach ($f in $expected) {
    if (-not (Test-Path (Join-Path $build $f))) { Fail "Build finished but build\$f is missing." }
}

# ------------------------------------------------------------------ stage dist\
Write-Host "`n== Staging dist\ ==" -ForegroundColor Cyan
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
foreach ($d in 'examples', 'include', 'tools', 'tools\l10n') {
    New-Item -ItemType Directory -Force (Join-Path $dist $d) | Out-Null
}

Copy-Item (Join-Path $build 'd3dcompiler_47.dll'), (Join-Path $build 'DWrite.dll') $dist
Copy-Item (Join-Path $build 'HelloMod.dll') (Join-Path $dist 'examples')
Copy-Item (Join-Path $build 'fh2mod.exe')   (Join-Path $dist 'tools')
Copy-Item (Join-Path $root 'include\fh2mod*.h') (Join-Path $dist 'include')
Copy-Item (Join-Path $root 'include\fh2game.h') (Join-Path $dist 'include')
Copy-Item (Join-Path $root 'tools\l10n\*.py')   (Join-Path $dist 'tools\l10n')
Copy-Item (Join-Path $root 'scripts\dist-readme.txt') (Join-Path $dist 'README.txt')
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $dist 'LICENSE.txt')
Copy-Item (Join-Path $root 'third_party\minhook\LICENSE.txt') (Join-Path $dist 'LICENSE-MinHook.txt')

Get-ChildItem $dist -Recurse -File | ForEach-Object {
    '  ' + $_.FullName.Substring($dist.Length + 1)
}

# -------------------------------------------------------------- optional install
if ($InstallTo) {
    Write-Host "`n== Installing to $InstallTo ==" -ForegroundColor Cyan
    if (-not (Test-Path (Join-Path $InstallTo 'azimuth.exe'))) {
        Fail "azimuth.exe not found in '$InstallTo'. Point -InstallTo at the folder that contains it."
    }
    Copy-Item (Join-Path $dist 'd3dcompiler_47.dll'), (Join-Path $dist 'DWrite.dll') $InstallTo -Force
    $mods = Join-Path $InstallTo 'fh2-mods'
    New-Item -ItemType Directory -Force $mods | Out-Null
    if ($IncludeExample) {
        Copy-Item (Join-Path $dist 'examples\HelloMod.dll') $mods -Force
        Write-Host "  Copied HelloMod.dll into fh2-mods"
    }
    Write-Host "  Platform DLLs installed. Launch the game via Azimuth, then read:"
    Write-Host "  $mods\platform.log"
}

Write-Host "`nDone. Output is in $dist" -ForegroundColor Green
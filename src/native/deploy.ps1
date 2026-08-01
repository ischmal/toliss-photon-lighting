<#
  Build the native ToLiss Photon .xpl and deploy it into X-Plane 12 (Windows).

  Run from anywhere:
      powershell -ExecutionPolicy Bypass -File src\native\deploy.ps1
  or just double-click deploy.bat.

  Options:
    -XPlaneRoot <path>   X-Plane 12 folder (default: the Steam install)
    -NoBuild             skip the build; just copy the already-built .xpl
    -Config <cfg>        build config (default: Release)
    -Dev                 build with the DEV TOOLING in: the Plugins > ToLiss
                         Photon > Dev menu and its window (profiles, panel probe,
                         FX compositor + history, repo build commands, log).
                         The probe can paint over the displays - never ship it.
                         Re-run without -Dev to get a clean plugin back.

  The two builds go to SEPARATE folders (build/ and build-dev/), so switching
  between them is a copy rather than a full rebuild.

  Notes: X-Plane must be CLOSED (a loaded .xpl is locked and can't be overwritten).
  Windows/win_x64 only.
#>
[CmdletBinding()]
param(
    [string] $XPlaneRoot = "C:\Program Files (x86)\Steam\steamapps\common\X-Plane 12",
    [switch] $NoBuild,
    [string] $Config = "Release",
    [switch] $Dev
)

$ErrorActionPreference = 'Stop'
$here  = Split-Path -Parent $MyInvocation.MyCommand.Path
# Separate build trees per flavour. One tree would mean a full recompile every
# time you flip -Dev, which is the friction this whole pass is about removing.
$build = Join-Path $here $(if ($Dev) { 'build-dev' } else { 'build' })
$xpl   = Join-Path $build 'ToLissPhoton\win_x64\ToLissPhoton.xpl'

function Find-CMake {
    $c = (Get-Command cmake -ErrorAction SilentlyContinue).Source
    if ($c) { return $c }
    $known = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (Test-Path $known) { return $known }
    $found = Get-ChildItem "C:\Program Files\Microsoft Visual Studio" -Recurse -Filter cmake.exe -ErrorAction SilentlyContinue |
             Where-Object { $_.FullName -match 'CMake\\CMake\\bin' } | Select-Object -First 1
    if ($found) { return $found.FullName }
    throw "cmake not found on PATH or in Visual Studio. Install CMake, or add it to PATH."
}

if (-not $NoBuild) {
    $cmake = Find-CMake
    # Always configure, and always state PHOTON_DEV explicitly. A cached ON would
    # otherwise be sticky: every later plain deploy would keep shipping a plugin
    # that can paint magenta over the captain's PFD.
    $devArg = if ($Dev) { "-DPHOTON_DEV=ON" } else { "-DPHOTON_DEV=OFF" }
    if ($Dev) {
        Write-Host "DEV BUILD - Dev menu and panel probe are IN. Do not ship." -ForegroundColor Yellow
    }
    Write-Host "Configuring..." -ForegroundColor Cyan
    & $cmake -S $here -B $build -A x64 $devArg
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed (exit $LASTEXITCODE)" }
    Write-Host "Building $Config..." -ForegroundColor Cyan
    & $cmake --build $build --config $Config
    if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }
}

if (-not (Test-Path $xpl)) {
    throw "built plugin not found: $xpl  (build it first, or drop -NoBuild)"
}
if (-not (Test-Path (Join-Path $XPlaneRoot 'Resources\plugins'))) {
    throw "not an X-Plane 12 folder (no Resources\plugins): $XPlaneRoot  (pass -XPlaneRoot)"
}
if (Get-Process -Name 'X-Plane' -ErrorAction SilentlyContinue) {
    throw "X-Plane is running. Close it fully, then re-run - a loaded .xpl is locked and cannot be overwritten."
}

$dst = Join-Path $XPlaneRoot 'Resources\plugins\ToLissPhoton\win_x64\ToLissPhoton.xpl'
New-Item -ItemType Directory -Force -Path (Split-Path $dst) | Out-Null
Copy-Item $xpl $dst -Force
$info = Get-Item $dst
Write-Host ("deployed -> {0}" -f $dst) -ForegroundColor Green
Write-Host ("           {0:N0} bytes, {1}" -f $info.Length, $info.LastWriteTime)

# Starter overlay images for the Panel FX compositor. Copied by NAME, never as a
# folder mirror: the same folder is where you drop your own images, and a
# Copy-Item of the whole tree with -Force is one typo away from deleting them.
# Regenerate the sources with `python build/make_overlays.py`.
$overlaySrc = Join-Path $here 'overlays'
if (Test-Path $overlaySrc) {
    $overlayDst = Join-Path $XPlaneRoot 'Resources\plugins\ToLissPhoton\overlays'
    New-Item -ItemType Directory -Force -Path $overlayDst | Out-Null
    $n = 0
    Get-ChildItem $overlaySrc -File | ForEach-Object {
        Copy-Item $_.FullName (Join-Path $overlayDst $_.Name) -Force
        $n++
    }
    Write-Host ("           {0} starter overlay image(s) -> {1}" -f $n, $overlayDst)
}

if ($Dev) {
    Write-Host "           DEV build - Plugins > ToLiss Photon > Dev > Dev window..." -ForegroundColor Yellow
}

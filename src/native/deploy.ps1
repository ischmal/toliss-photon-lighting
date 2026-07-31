<#
  Build the native ToLiss Photon .xpl and deploy it into X-Plane 12 (Windows).

  Run from anywhere:
      powershell -ExecutionPolicy Bypass -File src\native\deploy.ps1
  or just double-click deploy.bat.

  Options:
    -XPlaneRoot <path>   X-Plane 12 folder (default: the Steam install)
    -NoBuild             skip the build; just copy the already-built .xpl
    -Config <cfg>        build config (default: Release)
    -Probe               build the EXPERIMENTAL panel-FBO draw-order probe
                         (paints magenta over the displays - never ship it).
                         Re-run without -Probe to get a clean plugin back.

  Notes: X-Plane must be CLOSED (a loaded .xpl is locked and can't be overwritten).
  Windows/win_x64 only.
#>
[CmdletBinding()]
param(
    [string] $XPlaneRoot = "C:\Program Files (x86)\Steam\steamapps\common\X-Plane 12",
    [switch] $NoBuild,
    [string] $Config = "Release",
    [switch] $Probe
)

$ErrorActionPreference = 'Stop'
$here  = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $here 'build'
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
    # Always configure, and always state PHOTON_PANEL_PROBE explicitly. A cached ON
    # would otherwise be sticky: every later plain deploy would keep shipping a
    # plugin that paints magenta over the captain's PFD.
    $probeArg = if ($Probe) { "-DPHOTON_PANEL_PROBE=ON" } else { "-DPHOTON_PANEL_PROBE=OFF" }
    if ($Probe) {
        Write-Host "PANEL PROBE BUILD - experimental, do not ship." -ForegroundColor Yellow
    }
    Write-Host "Configuring..." -ForegroundColor Cyan
    & $cmake -S $here -B $build -A x64 $probeArg
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

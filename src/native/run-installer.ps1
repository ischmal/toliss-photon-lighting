<#
  Build and RUN the installer. This is the one command.

      powershell -ExecutionPolicy Bypass -File src\native\run-installer.ps1

  The counterpart to deploy.ps1: that one puts the plugin and the OBJs into
  X-Plane, this one builds the thing users double-click and starts it. If a
  session tells you to run cmake by hand, add %USERPROFILE%\.cargo\bin to PATH, or
  remember a --payload-dir, that session is working from a stale recipe.

  WHY A SCRIPT AND NOT A DOC: the steps genuinely change as the installer is
  reshaped, and prose goes stale silently. Everything a test run needs -- which
  build tree, which cmake flags, where Rust is, whether a payload is staged, how
  to make a GUI-subsystem binary block a shell -- lives here, so "how do I test
  it?" has one answer that is executable and therefore checkable.

  What it does, in order:
    1. (-Test only) photoncore_tests + the Python suite; stops if either fails
    2. builds the PLUGIN (.xpl) into build/, so the payload can stage a current one
    3. stages release/payload/ when it is missing or stale (-Payload forces)
    4. cmake configure + build the INSTALLER, into the tree that matches the mode
    5. runs the binary, pointed at release/payload/

  MODES (pick one; default is the GUI)
    (none)               the Slint GUI. Needs Rust; builds into build-gui/.
    -Tui                 the terminal installer. No Rust; builds into build-core/.
    -Cli <args...>       a headless subcommand, e.g.
                           -Cli detect --json
                           -Cli status --aircraft "C:\...\ToLissA319_V1p0p12"
                         Waits for the process and returns its exit code, which a
                         bare call does NOT do -- see the note by Invoke-Installer.

  OPTIONS
    -DryRun              pass --dry-run: every action logged, nothing written.
                         THE DEFAULT WAY TO TEST AN INSTALL. Without it a run
                         edits the aircraft in your live X-Plane for real.
    -Payload             force a restage even when nothing looks stale.
    -Test                run both suites first and stop if either fails.
    -NoPlugin            skip the .xpl build (step 2). Use when you are working on
                         the installer alone and know the staged plugin is current.
    -NoBuild             skip cmake for the INSTALLER; run whatever is built.
    -NoRun               build (and stage) but do not launch.
    -Config <cfg>        build config (default: Release).
    -Clean               delete the build tree for the chosen mode first. The
                         Slint tree is expensive to rebuild -- only use it when a
                         cmake cache is actually wrong.

  ⚠ THE PAYLOAD IS WHAT MAKES AN INSTALL POSSIBLE. `photon-installer` resolves it
  from its own folder, and a freshly built binary in build-gui\Release has none --
  detection and every screen still work, but installing fails with "this build is
  incomplete". This script passes --payload-dir automatically when
  release/payload/ exists, and tells you when it does not.

  ⚠ THE PLUGIN IS BUILT AND RESTAGED AUTOMATICALLY, AND THAT IS A 2026-08-14 FIX
  FOR A REAL STALE INSTALL. Two things went wrong together and each hid the other:

    * `-Payload` ran `make_release.py --payload-only`, whose --out defaulted to the
      REPO ROOT -- so it staged <repo>\payload while this script ran the binary
      against release\payload. Restaging updated a tree nothing read. One payload
      location now, release\payload, for every mode.
    * Nothing built the .xpl. make_release COPIES one out of
      src\native\build\ToLissPhoton, and the only thing that ever refreshed that
      folder was deploy.ps1 -- so an installer test run happily installed whatever
      plugin was last deployed to the sim, hours or days old.

  Neither has a symptom. The installer reports success, the files land, X-Plane
  loads a plugin -- last week's. So: step 2 builds the .xpl in its own tree (the
  installer's build-gui/build-core stay PHOTON_CORE_ONLY, which is what keeps
  installer work possible with no X-Plane SDK), step 3 restages when the staged
  copy is older than the built one, and `make_release.py` REFUSES outright to
  stage an .xpl older than its own sources.

  ⚠ THE .xpl BUILD NEEDS THE X-PLANE SDK at src\native\SDK. Without it step 2 is
  SKIPPED WITH A WARNING rather than failing: working on the installer with no SDK
  is a supported thing to do (that is what PHOTON_CORE_ONLY is for), and the
  payload keeps whatever plugin it already had.

  ⚠ THE GUI BUILD NEEDS RUST, and on this machine Rust is installed but NOT on
  PATH (it lives in %USERPROFILE%\.cargo\bin). The script adds it. A shell without
  it reports "rustc: command not found", which reads as "not installed" and sends
  you to reinstall a toolchain you already have.

  Windows/win_x64 only. See docs/installer_gui_plan.md.
#>
# ⚠ PositionalBinding = $false IS LOAD-BEARING. Without it every parameter gets an
# implicit position in declaration order, so `-Cli detect --json` binds "detect" to
# the first positional [string] -- $Config -- and the script cheerfully tries to
# build a configuration called "detect". The subcommand must reach $Rest and
# nothing else may claim it.
[CmdletBinding(PositionalBinding = $false)]
param(
    [switch] $Tui,
    [switch] $Cli,
    [switch] $DryRun,
    [switch] $Payload,
    [switch] $Test,
    [switch] $NoPlugin,
    [switch] $NoBuild,
    [switch] $NoRun,
    [switch] $Clean,
    [string] $Config = "Release",
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $Rest = @()
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Split-Path -Parent (Split-Path -Parent $here)   # src/native -> src -> repo

# The GUI and the no-GUI binary use SEPARATE trees, like deploy.ps1's build/ and
# build-dev/. One tree would mean recompiling Slint every time you flip -Tui.
$gui   = -not $Tui
$build = Join-Path $here $(if ($gui) { 'build-gui' } else { 'build-core' })
$exe   = Join-Path $build "$Config\photon-installer.exe"

# ⚠ A THIRD TREE, AND THE PLUGIN'S ALONE. build/ is deploy.ps1's -- the shipping
# (PHOTON_DEV=OFF) .xpl, and the folder make_release.py copies from by default.
# The .xpl is NOT built into build-gui/build-core: those are configured
# PHOTON_CORE_ONLY so installer work needs no X-Plane SDK, and putting the plugin
# in with them would make Slint and the SDK each other's prerequisite.
$pluginBuild = Join-Path $here 'build'
$pluginDir   = Join-Path $pluginBuild 'ToLissPhoton'
$sdkHeader   = Join-Path $here 'SDK\CHeaders\XPLM\XPLMDefs.h'
$payloadDir  = Join-Path $repo 'release\payload'
$stagedXpl   = Join-Path $payloadDir 'plugin\ToLissPhoton\win_x64\ToLissPhoton.xpl'
$builtXpl    = Join-Path $pluginDir 'win_x64\ToLissPhoton.xpl'

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

# See the ⚠ in the header: installed, but not on PATH.
function Add-RustToPath {
    if (Get-Command cargo -ErrorAction SilentlyContinue) { return }
    $cargoBin = Join-Path $env:USERPROFILE '.cargo\bin'
    if (Test-Path (Join-Path $cargoBin 'cargo.exe')) {
        $env:PATH = "$cargoBin;$env:PATH"
        return
    }
    throw @"
The GUI build needs Rust (>= 1.92) and cargo was not found.
Looked on PATH and in $cargoBin.
  winget install Rustlang.Rustup      (or https://rustup.rs)
Or build the terminal installer instead, which needs no Rust:
  run-installer.ps1 -Tui
"@
}

# Same Store-alias trap deploy.ps1 documents: a stub `python.exe` resolves, opens
# the Microsoft Store and exits 9009.
function Find-Python {
    foreach ($cand in @(@{ Exe = 'python'; Pre = @() }, @{ Exe = 'py'; Pre = @('-3') })) {
        $src = (Get-Command $cand.Exe -ErrorAction SilentlyContinue).Source
        if (-not $src) { continue }
        & $src @($cand.Pre) -c "import sys" *> $null
        if ($LASTEXITCODE -eq 0) { return @{ Exe = $src; Pre = $cand.Pre } }
    }
    throw "python not found on PATH (tried 'python' and 'py -3')."
}

function Invoke-RepoPython {
    param([hashtable] $Py, [string[]] $PyArgs, [string] $What)
    Write-Host $What -ForegroundColor Cyan
    Push-Location $repo
    try {
        & $Py.Exe @($Py.Pre) -u @PyArgs
        if ($LASTEXITCODE -ne 0) { throw "$What failed (exit $LASTEXITCODE)" }
    } finally { Pop-Location }
}

# ⚠ A GUI-SUBSYSTEM PROCESS DOES NOT BLOCK THE SHELL, so `& $exe detect --json`
# returns to the prompt immediately and $LASTEXITCODE is empty. Start-Process
# -Wait is what makes a headless run behave like a normal command; exit codes DO
# propagate through it (verified 0 on success, 1 on a missing --aircraft).
function Invoke-Installer {
    param([string[]] $InstallerArgs, [switch] $Wait)
    Write-Host ("Running: photon-installer {0}" -f ($InstallerArgs -join ' ')) -ForegroundColor Cyan
    if ($Wait) {
        $p = Start-Process -FilePath $exe -ArgumentList $InstallerArgs -NoNewWindow -Wait -PassThru
        return $p.ExitCode
    }
    Start-Process -FilePath $exe -ArgumentList $InstallerArgs | Out-Null
    return 0
}

# ---- 1. tests ---------------------------------------------------------------
if ($Test) {
    $py = Find-Python
    $testExe = Join-Path $build "$Config\photoncore_tests.exe"
    if (Test-Path $testExe) {
        Write-Host "Running photoncore_tests..." -ForegroundColor Cyan
        & $testExe
        if ($LASTEXITCODE -ne 0) { throw "photoncore_tests failed (exit $LASTEXITCODE)" }
    } else {
        Write-Host "photoncore_tests not built yet - it will run on the next -Test." -ForegroundColor Yellow
    }
    Invoke-RepoPython $py @('-m','unittest','discover','-s','tests') "Running the Python suite..."
}

# ---- 2. the plugin ----------------------------------------------------------
# ⚠ See the header. The payload's .xpl is COPIED from $pluginDir by make_release,
# and before this step existed the only thing that ever wrote that folder was
# deploy.ps1 -- so testing the installer installed whatever plugin had last been
# deployed to the sim.
if (-not $NoPlugin) {
    if (-not (Test-Path $sdkHeader)) {
        Write-Host ""
        Write-Host "X-Plane SDK not found at $sdkHeader" -ForegroundColor Yellow
        Write-Host "Skipping the plugin build - the payload keeps the .xpl it already has." -ForegroundColor Yellow
        Write-Host "Installer work does not need the SDK; a plugin TEST does." -ForegroundColor DarkGray
    } else {
        $cmake = Find-CMake
        # PHOTON_DEV stated explicitly for the reason deploy.ps1 states it: a
        # cached ON is sticky, and a dev .xpl in a payload is a build that can
        # paint magenta over the captain's PFD, handed to a tester.
        Write-Host "Building the plugin (.xpl, Release)..." -ForegroundColor Cyan
        & $cmake -S $here -B $pluginBuild -A x64 -DPHOTON_DEV=OFF
        if ($LASTEXITCODE -ne 0) { throw "plugin cmake configure failed (exit $LASTEXITCODE)" }
        # ONLY the .xpl target: this tree also holds a photon-installer and the
        # core tests, and rebuilding those here would duplicate step 4 for nothing.
        & $cmake --build $pluginBuild --config $Config --target ToLissPhoton
        if ($LASTEXITCODE -ne 0) { throw "plugin build failed (exit $LASTEXITCODE)" }
    }
}

# ---- 3. payload -------------------------------------------------------------
# Restaged when it is MISSING or STALE, not only when asked. -Payload is now the
# force, not the correctness switch: "you must remember a flag or your install is
# silently a day old" is the bug this whole section exists to close.
#
# ⚠ The plugin is compared to its OWN staged copy, not to the payload's newest
# file. copytree preserves mtimes, so the staged .xpl carries its build time and
# the two are directly comparable -- and the payload-wide newest is exactly the
# wrong stamp for it, since freshly built OBJs would mask a year-old .xpl.
function Get-NewestWrite {
    param([string[]] $Paths)
    $newest = [datetime]::MinValue
    foreach ($p in $Paths) {
        if (-not (Test-Path $p)) { continue }
        $item = Get-Item $p
        if ($item.PSIsContainer) {
            # __pycache__ is excluded: a .pyc rewritten by any python run is not
            # a payload input, and "a payload input changed" is a message that
            # has to be true or nobody reads it.
            $f = Get-ChildItem $p -Recurse -File -ErrorAction SilentlyContinue |
                 Where-Object { $_.FullName -notmatch '\\__pycache__\\' } |
                 Sort-Object LastWriteTime -Descending | Select-Object -First 1
            if ($f -and $f.LastWriteTime -gt $newest) { $newest = $f.LastWriteTime }
        } elseif ($item.LastWriteTime -gt $newest) { $newest = $item.LastWriteTime }
    }
    return $newest
}

function Get-PayloadStaleReason {
    if (-not (Test-Path $payloadDir)) { return "no payload staged yet" }
    if ((Test-Path $builtXpl) -and (-not (Test-Path $stagedXpl))) {
        return "a plugin has been built but none is staged"
    }
    if ((Test-Path $builtXpl) -and (Test-Path $stagedXpl)) {
        $built  = (Get-Item $builtXpl).LastWriteTime
        $staged = (Get-Item $stagedXpl).LastWriteTime
        if ($staged -lt $built) {
            return ("the staged plugin is older than the built one ({0:yyyy-MM-dd HH:mm} vs {1:yyyy-MM-dd HH:mm})" -f $staged, $built)
        }
    }
    # The payload's OTHER inputs. Coarse on purpose -- a spurious restage costs
    # seconds and a missed one costs a debugging session.
    $inputs = @(
        (Join-Path $repo 'src\lights'),
        (Join-Path $repo 'build'),
        (Join-Path $here 'overlays')
    )
    $newestIn  = Get-NewestWrite $inputs
    $newestOut = Get-NewestWrite @($payloadDir)
    if ($newestIn -gt $newestOut) {
        return ("a payload input changed at {0:yyyy-MM-dd HH:mm}, after the last staging" -f $newestIn)
    }
    return $null
}

$staleReason = Get-PayloadStaleReason
if ($Payload -or $staleReason) {
    $why = if ($Payload -and -not $staleReason) { "forced with -Payload" } else { $staleReason }
    $py = Find-Python
    Invoke-RepoPython $py @('build/make_release.py','--payload-only') "Staging release/payload ($why)..."
} else {
    Write-Host "Payload is current - not restaging (-Payload forces one)." -ForegroundColor DarkGray
}

# ---- 4. build the installer -------------------------------------------------
if ($Clean -and (Test-Path $build)) {
    Write-Host "Removing $build..." -ForegroundColor Yellow
    Remove-Item $build -Recurse -Force
}

if (-not $NoBuild) {
    if ($gui) { Add-RustToPath }
    $cmake = Find-CMake
    # ⚠ ALWAYS STATED EXPLICITLY, never left to the cache. A sticky PHOTON_GUI=ON
    # in a shared tree is how a "plain" build quietly keeps needing Rust; the same
    # reasoning as deploy.ps1 stating PHOTON_DEV every time.
    $guiArg = if ($gui) { "-DPHOTON_GUI=ON" } else { "-DPHOTON_GUI=OFF" }
    # ⚠ PHOTON_CORE_ONLY skips the .xpl, so no X-Plane SDK is needed to work on
    # the installer -- and that stays true now that this script builds the plugin
    # too, because the plugin goes in its OWN tree (step 2). Turning it off here
    # to "get both in one build" would make the SDK a hard requirement for every
    # installer change and drag Slint into every plugin change.
    Write-Host ("Configuring ({0})..." -f $(if ($gui) { 'GUI, Slint' } else { 'no GUI' })) -ForegroundColor Cyan
    if ($gui) { Write-Host "  first Slint build compiles from source - several minutes, then cached." -ForegroundColor DarkGray }
    & $cmake -S $here -B $build -A x64 $guiArg -DPHOTON_CORE_ONLY=ON
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed (exit $LASTEXITCODE)" }
    Write-Host "Building $Config..." -ForegroundColor Cyan
    & $cmake --build $build --config $Config
    if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }
}

if (-not (Test-Path $exe)) {
    throw "installer not built: $exe  (build it, or drop -NoBuild)"
}

$info = Get-Item $exe
Write-Host ("built -> {0}" -f $exe) -ForegroundColor Green
Write-Host ("         {0:N1} MB, {1}" -f ($info.Length / 1MB), $info.LastWriteTime)

# ---- 5. run -----------------------------------------------------------------
# ⚠ $runArgs, not $args. `$args` is a PowerShell AUTOMATIC variable (a function's
# own unbound arguments), and assigning to it is a trap waiting for the first
# helper this file grows.
$runArgs = @()
if (Test-Path $payloadDir) {
    $runArgs += @('--payload-dir', $payloadDir)
    # ⚠ THE .xpl THIS RUN WILL INSTALL, named on screen right before the run.
    # There is no other way to see it: the installer reports success either way,
    # and the plugin's own version only moves between releases, so a plugin days
    # older than the source reads as a perfectly good install until someone
    # notices a fix is missing. That is the whole bug this line exists for.
    if (Test-Path $stagedXpl) {
        $x = Get-Item $stagedXpl
        Write-Host ("payload -> {0}" -f $payloadDir) -ForegroundColor Green
        Write-Host ("           ToLissPhoton.xpl  {0:N2} MB, built {1}" -f ($x.Length / 1MB), $x.LastWriteTime)
    } else {
        Write-Host ""
        Write-Host "Payload has NO plugin - an install will lay down OBJs with no .xpl." -ForegroundColor Yellow
        Write-Host "Build it (drop -NoPlugin, or check the X-Plane SDK is at src\native\SDK)." -ForegroundColor Yellow
    }
} else {
    Write-Host ""
    Write-Host "No payload staged at release\payload - DETECTION WORKS BUT INSTALLING WILL FAIL." -ForegroundColor Yellow
    Write-Host "Re-run with -Payload to stage it." -ForegroundColor Yellow
}
if ($DryRun) { $runArgs += '--dry-run' }

if ($NoRun) {
    Write-Host ""
    Write-Host "Not launched (-NoRun)." -ForegroundColor Yellow
    return
}

if ($Cli) {
    if (-not $Rest -or $Rest.Count -eq 0) {
        throw "-Cli needs a subcommand, e.g. -Cli detect --json"
    }
    # The subcommand FIRST: IsCliInvocation only looks at args[1].
    $exitCode = Invoke-Installer ($Rest + $runArgs) -Wait
    Write-Host ("exit {0}" -f $exitCode) -ForegroundColor $(if ($exitCode -eq 0) { 'Green' } else { 'Red' })
    exit $exitCode
}

if ($Tui) { $runArgs += '--tui' }
if ($DryRun) {
    Write-Host "DRY RUN - the installer will log every action and write nothing." -ForegroundColor Yellow
} else {
    Write-Host "NOT a dry run - this WILL modify aircraft in your X-Plane install. Ctrl-C now, or pass -DryRun." -ForegroundColor Yellow
}
# The TUI draws in this console and must run in the foreground; the GUI opens its
# own window, so waiting on it would just block the shell for no reason.
[void] (Invoke-Installer $runArgs -Wait:$Tui)

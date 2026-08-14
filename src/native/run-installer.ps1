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
    2. (-Payload only) stages release/payload/ -- make_release.py --payload-only
    3. cmake configure + build, into the tree that matches the mode
    4. runs the binary, pointed at release/payload/ if one is staged

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
    -Payload             restage release/payload/ before running. Needed after a
                         .phdsl or overlay change; the installer reads OBJs from
                         there, not from dist/.
    -Test                run both suites first and stop if either fails.
    -NoBuild             skip cmake; run whatever is already built.
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

# ---- 2. payload -------------------------------------------------------------
$payloadDir = Join-Path $repo 'release\payload'
if ($Payload) {
    $py = Find-Python
    Invoke-RepoPython $py @('build/make_release.py','--payload-only') "Staging release/payload..."
}

# ---- 3. build ---------------------------------------------------------------
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
    # PHOTON_CORE_ONLY skips the .xpl, so no X-Plane SDK is needed to work on the
    # installer. deploy.ps1 is what builds the plugin.
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

# ---- 4. run -----------------------------------------------------------------
$args = @()
if (Test-Path $payloadDir) {
    $args += @('--payload-dir', $payloadDir)
} else {
    Write-Host ""
    Write-Host "No payload staged at release\payload - DETECTION WORKS BUT INSTALLING WILL FAIL." -ForegroundColor Yellow
    Write-Host "Re-run with -Payload to stage it." -ForegroundColor Yellow
}
if ($DryRun) { $args += '--dry-run' }

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
    $exitCode = Invoke-Installer ($Rest + $args) -Wait
    Write-Host ("exit {0}" -f $exitCode) -ForegroundColor $(if ($exitCode -eq 0) { 'Green' } else { 'Red' })
    exit $exitCode
}

if ($Tui) { $args += '--tui' }
if ($DryRun) {
    Write-Host "DRY RUN - the installer will log every action and write nothing." -ForegroundColor Yellow
} else {
    Write-Host "NOT a dry run - this WILL modify aircraft in your X-Plane install. Ctrl-C now, or pass -DryRun." -ForegroundColor Yellow
}
# The TUI draws in this console and must run in the foreground; the GUI opens its
# own window, so waiting on it would just block the shell for no reason.
[void] (Invoke-Installer $args -Wait:$Tui)

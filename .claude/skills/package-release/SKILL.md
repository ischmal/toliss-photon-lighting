---
name: package-release
description: Packaging a tester or release ZIP of the compiled photon-installer (make_release.py --bundle) and verifying the artifact. Load before building a release/tester bundle, when asked to "package a release", or when verifying/debugging a built bundle. Local single-platform path only — multi-platform releases go through CI.
---

# Packaging an installer release bundle

Builds `release/ToLissPhoton-Installer-v<VER>-<OS>.zip` — one folder holding
`photon-installer` + `data/` + `README.txt` — from the current working tree.
First run of this pipeline: the 0.8.1 tester drop, 2026-08-09.

**Scope: the LOCAL, single-platform bundle** (tester drops, local verification).
Multi-platform releases are CI's job — `workflow_dispatch` on `main` produces all
three OS bundles without publishing; a `v*` tag publishes. Do not hand-build
non-Windows bundles here.

## The pipeline

1. **Pre-flight.** `python -m unittest tests.test_version -v` (pins every
   version reader to `core/version.h`, THE version). Then `git status --short` —
   ⚠ **the bundle ships the WORKING TREE, uncommitted changes included.** That is
   the point of a tester drop, but say what's in it when reporting the result.

2. **Build Release.** cmake is not on `PATH` on this machine — it lives inside
   VS 18 Community:

   ```powershell
   & "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" `
       --build src/native/build --config Release
   ```

   (No build tree yet: configure first with `-S src/native -B src/native/build -A x64`.)
   ⚠ **Package from `src/native/build`, never `build-dev`** — the dev tree is
   `PHOTON_DEV` and must not ship. The separate trees make this safe by
   construction; keep it that way.

3. **Tests.** `./src/native/build/Release/photoncore_tests.exe` (includes
   `install_tests.cpp` — the only coverage over detect/actions/payload since the
   Python installer's deletion), then `python -m unittest discover -s tests`.
   ⚠ **Redirect the Python suite to a log file and grep the `Ran/OK` line** —
   piped output truncates under buffering and a cut-off log reads as a pass.

4. **Stage + zip.**

   ```bash
   rm -rf release
   python build/make_release.py --bundle
   ```

   Defaults find the plugin (`src/native/build/ToLissPhoton`) and the installer
   exe on their own; `--plugin-dir`/`--installer-exe` override. ⚠ `rm -rf release`
   can throw a transient `WinError 5` under OneDrive/AV — just retry once.

5. **Verify the artifact** (each of these has failed silently at least once):
   - Layout: ONE top-level folder — `photon-installer.exe`, `data/{objs,
     textures, plugin/<arch>, plugindata, realwings_patch.json}`, `README.txt`.
   - Size sanity: **~45 MB** (Gus's textures are ~58 MiB of the payload). A tiny
     ZIP means the payload is missing, not that compression got lucky.
   - Exec bit: `0755` in the ZIP metadata (`external_attr`), so the macOS/Linux
     equivalents extract runnable.
   - Extract it and run **from the extracted folder**: `photon-installer version`
     must report the release version, `detect --json` must resolve the sibling
     `data/` (the bundle name for `payload/` — the binary accepts both).

6. **Smoke test** — fake X-Plane tree, then with the *extracted* binary:
   `status` → `install --json` → `status` → `uninstall --json`, asserting the
   tree comes back **byte-for-byte** (hash every file before/after).

## Smoke-test tripwires (each cost a debugging round on the first run)

- ⚠ **Use a SHORT scratch path** (e.g. `C:\Users\<u>\AppData\Local\Temp\ptest`),
  never the session scratchpad. A deeply nested path hits Windows `MAX_PATH`
  inside the backup tree: backups silently fail and the uninstall "leaves files
  behind" — which reads exactly like a product bug and is not.
- ⚠ **`_spot_name_3d/* = none` is the PATCHED state, not stock** (the `.acf`
  cockpit-spot sentinel). A fixture authored in that state makes the reversal
  look broken; an assertion written the other way round is inverted.
- Keep the pristine-tree snapshot **outside** any folder an installer command
  might touch — on the first run the comparison baseline itself got patched.
- For iterating on the binary without restaging ~58 MiB of textures each build:
  stage once (`make_release.py --payload-only`), then run with
  `--payload-dir` pointed at it (`docs/installer_cpp_plan.md` §10a).

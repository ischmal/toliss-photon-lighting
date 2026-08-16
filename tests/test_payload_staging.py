"""The payload staging path — one location, and a plugin no older than its source.

⚠ THE BUG THIS CLOSES (2026-08-14): a test install laid down a day-old `.xpl`.
Two independent faults, and each hid the other.

  * `make_release.py --payload-only` defaulted `--out` to the REPO ROOT, so it
    staged `<repo>/payload/` — while `run-installer.ps1` ran the binary with
    `--payload-dir release\\payload`. `-Payload` faithfully rebuilt one tree and
    every install read the other, which held whatever an earlier full run left.
  * Nothing built the plugin. `stage_plugin` COPIES one out of
    `src/native/build/ToLissPhoton`, and the only thing that ever wrote that
    folder was `deploy.ps1` — so testing the installer installed whatever plugin
    had last been deployed to the sim.

Neither has a symptom. The installer reports success, the files land, X-Plane
loads a plugin; only the About tab's version says it is last week's, and that
reads as a half-finished install. Every test here is a guard against a silent
wrong-artifact, not against a crash.
"""
from __future__ import annotations

import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[1]
MAKE_RELEASE = REPO / "build" / "make_release.py"
RUN_INSTALLER = REPO / "src" / "native" / "run-installer.ps1"

MR = MAKE_RELEASE.read_text(encoding="utf-8", errors="replace")
PS = RUN_INSTALLER.read_text(encoding="utf-8", errors="replace")


def _pyfn(name, text=MR):
    """A top-level def, from its signature to the next top-level statement."""
    m = re.search(r"\ndef %s\(.*?(?=\n\S)" % re.escape(name), text, re.S)
    assert m, "%s moved or was renamed" % name
    return m.group(0)


class OnePayloadLocationTests(unittest.TestCase):
    """⚠ TWO STAGING ROOTS IS THE BUG; ONE IS THE FIX. Whatever the location is,
    the writer and the reader have to name the same one — and the only way to be
    sure of that from here is to check that neither has an opinion of its own."""

    def test_payload_only_stages_where_every_other_mode_does(self):
        """No `--payload-only ? REPO : REPO / "release"` ternary any more. The
        old default put a ~57 MiB tree in the repo root, which `release/` being
        gitignored and the repo root not is a second reason against."""
        body = _pyfn("main")
        m = re.search(r"out = .*?\n", body)
        self.assertIsNotNone(m)
        self.assertNotIn("payload_only", m.group(0),
                         "--payload-only picks a different --out again: %s"
                         % m.group(0).strip())
        self.assertIn('REPO / "release"', m.group(0))

    def test_the_script_reads_the_tree_the_script_writes(self):
        """The reader's path, spelled once and used for both the restage check
        and --payload-dir. This is the actual defect: the two were different
        strings in two places and nothing compared them."""
        m = re.search(r"\$payloadDir\s*=\s*Join-Path \$repo '([^']+)'", PS)
        self.assertIsNotNone(m, "$payloadDir moved or was renamed")
        self.assertEqual(m.group(1).replace("\\", "/"), "release/payload")
        self.assertIn("$runArgs += @('--payload-dir', $payloadDir)", PS,
                      "the binary is pointed at something other than $payloadDir")

    def test_the_script_passes_no_out_of_its_own(self):
        """It must inherit make_release's default rather than restate it — a
        second statement of the location is a second thing to get wrong, which
        is how this went wrong the first time."""
        m = re.search(r"@\('build/make_release\.py'[^)]*\)", PS)
        self.assertIsNotNone(m, "the make_release invocation moved")
        self.assertNotIn("--out", m.group(0))

    def test_nothing_still_points_at_the_repo_root_payload(self):
        """A leftover `--payload-dir payload` in a doc or script is the same bug
        again, wearing the old path.

        ⚠ docs/ is LOCAL-ONLY since 2026-08-15 (untracked as dev environment),
        so a missing path is skipped rather than read: on CI this test crashed
        with FileNotFoundError — which failed the whole v0.9 tag build — while
        passing on every dev machine, where the folder still exists."""
        offenders = []
        for path in [MAKE_RELEASE, RUN_INSTALLER,
                     REPO / "docs" / "installer.md",
                     REPO / "src" / "native" / "README.md"]:
            if not path.is_file():
                continue
            for n, line in enumerate(
                    path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                if re.search(r"--payload-dir\s+payload\b", line):
                    offenders.append("%s:%d" % (path.name, n))
        self.assertEqual(offenders, [],
                         "still naming the repo-root payload: %s" % offenders)


class PluginFreshnessTests(unittest.TestCase):
    """⚠ `make_release.py` COPIES a plugin; it never builds one. That is correct
    for CI, where the matrix builds the `.xpl` in the same job — and it is why
    the refusal has to live here rather than in whichever caller forgot."""

    def test_the_check_exists_and_compares_against_the_plugin_sources(self):
        self.assertIn("def check_plugin_freshness(", MR)
        m = re.search(r"PLUGIN_SOURCE_PATHS = \((.*?)\)", MR, re.S)
        self.assertIsNotNone(m, "PLUGIN_SOURCE_PATHS moved")
        listed = m.group(1)
        self.assertIn('"src" / "native" / "src"', listed)
        self.assertIn("CMakeLists.txt", listed)

    def test_the_installer_tree_is_not_a_plugin_source(self):
        """⚠ `src/installer/` is compiled into photon-installer only, never the
        `.xpl`, so no plugin rebuild can freshen the binary against an edit
        there — counted, the check fired on EVERY installer change (it did, on
        2026-08-15, the first installer-only edit after it shipped) and a check
        that always fires gets --allow-stale-plugin typed as a reflex."""
        m = re.search(r"PLUGIN_SOURCE_EXCLUDE = \((.*?)\)", MR, re.S)
        self.assertIsNotNone(m, "PLUGIN_SOURCE_EXCLUDE is gone")
        self.assertIn('"installer"', m.group(1))
        self.assertIn("_newest_mtime(PLUGIN_SOURCE_PATHS, PLUGIN_SOURCE_EXCLUDE)",
                      MR)

    def test_the_dsl_is_not_a_plugin_source(self):
        """⚠ It is a PAYLOAD input, restaged every run, and its being newer says
        nothing about whether the .xpl needs rebuilding. Listed here the check
        would fire on every .phdsl edit — and a check that always fires gets
        --allow-stale-plugin typed as a reflex."""
        listed = re.search(r"PLUGIN_SOURCE_PATHS = \((.*?)\)", MR, re.S).group(1)
        for payload_input in ('"lights"', '"overlays"', '"gus"'):
            self.assertNotIn(payload_input, listed)

    def test_it_refuses_rather_than_warns_by_default(self):
        body = _pyfn("check_plugin_freshness")
        self.assertIn("raise SystemExit", body)
        self.assertIn("if not allow_stale:", body)

    def test_it_cannot_accuse_when_it_cannot_see_the_sources(self):
        """A check that fires on a tree it failed to read is worse than none —
        it would refuse to stage anything from an unpacked source archive."""
        self.assertIn("if newest_src == 0.0:", _pyfn("check_plugin_freshness"))

    def test_the_escape_hatch_exists_and_is_scoped_to_foreign_binaries(self):
        """CI's merge job assembles a multi-arch folder from downloaded
        artifacts, whose mtimes mean nothing about the checkout. That is the one
        legitimate use, and the help text has to say so — it is the only thing
        standing between the flag and becoming the way past a rebuild."""
        m = re.search(r'"--allow-stale-plugin".*?\)\n', MR, re.S)
        self.assertIsNotNone(m, "--allow-stale-plugin moved")
        self.assertIn("CI artifacts", m.group(0))

    def test_the_check_runs_before_anything_is_deleted(self):
        """⚠ main() wipes the output tree before staging, so a refusal raised
        from stage_plugin — the LAST thing staged — cost the caller the payload
        they already had: nothing rebuilt, nothing left to run against, and a
        minute of texture copying wasted. The checks are pure reads."""
        body = _pyfn("main")
        self.assertIn("preflight_plugin(", body)
        self.assertLess(body.index("preflight_plugin("), body.index("_rmtree(payload)"),
                        "the freshness check runs after the payload is deleted")
        # ...and stage_plugin no longer carries its own copy of the decision.
        self.assertNotIn("allow_stale", _pyfn("stage_plugin"))

    def test_the_staged_plugin_reports_its_build_time(self):
        """The one fact that makes a stale plugin visible from outside. copytree
        preserves mtime, so this IS the binary's build time."""
        body = _pyfn("stage_plugin")
        self.assertIn("st.st_mtime", body)
        self.assertIn("built", body)

    def test_a_rmtree_that_survives_a_read_only_directory(self):
        """⚠ Not defensive programming. This repo lives under OneDrive, which
        leaves read-only reparse-point directories behind; `os.rmdir` on one
        fails with WinError 5 and `shutil.rmtree` gives up WHERE IT FAILED — so
        a restage died half way, having deleted half the payload and rebuilt
        none of it."""
        self.assertIn("def _rmtree(", MR)
        self.assertIn("onexc=_force_writable", MR)
        self.assertIn("stat.S_IWRITE", MR)
        # Every tree-removal in the file goes through it; the wrapper is the one
        # place shutil.rmtree may still be named.
        self.assertEqual(MR.count("shutil.rmtree("), 1,
                         "a raw shutil.rmtree is back")


class PluginBuildTests(unittest.TestCase):
    """The other half: something has to BUILD the .xpl the payload copies."""

    def test_the_script_builds_the_plugin_before_staging(self):
        self.assertLess(PS.index("--target ToLissPhoton"),
                        PS.index("Get-PayloadStaleReason"),
                        "the payload is staged before the plugin is built")

    def test_it_builds_into_its_own_tree(self):
        """⚠ NOT into build-gui/build-core. Those are configured
        PHOTON_CORE_ONLY so installer work needs no X-Plane SDK; building the
        plugin in with them would make Slint and the SDK each other's
        prerequisite in both directions."""
        self.assertRegex(PS, r"\$pluginBuild\s*=\s*Join-Path \$here 'build'")
        self.assertIn("-DPHOTON_CORE_ONLY=ON", PS)
        self.assertNotIn("-B $pluginBuild -A x64 -DPHOTON_CORE_ONLY", PS)

    def test_the_plugin_build_states_photon_dev(self):
        """A cached ON is sticky, and a dev .xpl handed to a tester is a build
        that can paint magenta over the captain's PFD. Same rule deploy.ps1
        states for the same reason."""
        m = re.search(r"cmake -S \$here -B \$pluginBuild[^\n]*", PS)
        self.assertIsNotNone(m, "the plugin configure line moved")
        self.assertIn("-DPHOTON_DEV=OFF", m.group(0))

    def test_a_missing_sdk_skips_rather_than_fails(self):
        """Working on the installer with no X-Plane SDK is supported — that is
        what PHOTON_CORE_ONLY is for — so the plugin build cannot be allowed to
        become a hard requirement of every installer change."""
        m = re.search(r"if \(-not \(Test-Path \$sdkHeader\)\) \{(.*?)\n    \} else \{",
                      PS, re.S)
        self.assertIsNotNone(m, "the SDK guard moved")
        self.assertNotIn("throw", m.group(1))
        self.assertIn("Skipping the plugin build", m.group(1))

    def test_restaging_is_automatic_and_the_flag_is_only_a_force(self):
        """⚠ "You must remember a flag or your install is silently a day old" is
        the bug, not the workflow."""
        self.assertIn("if ($Payload -or $staleReason) {", PS)
        self.assertIn("forced with -Payload", PS)

    def test_the_plugin_is_compared_against_its_own_staged_copy(self):
        """⚠ NOT against the payload's newest file. copytree preserves mtimes,
        so the staged .xpl carries its build time and the two are directly
        comparable — where a payload-wide stamp would let freshly built OBJs
        mask a year-old plugin, which is exactly the shape of the original bug."""
        body = re.search(r"function Get-PayloadStaleReason \{(.*?)\n\}", PS, re.S)
        self.assertIsNotNone(body, "Get-PayloadStaleReason moved")
        self.assertIn("$staged -lt $built", body.group(1))

    def test_the_run_names_the_plugin_it_is_about_to_install(self):
        """There is no other way to see it: the installer reports success either
        way and the plugin's version only moves between releases."""
        self.assertIn("ToLissPhoton.xpl  {0:N2} MB, built {1}", PS)

    def test_the_automatic_variable_is_not_shadowed(self):
        """`$args` is PowerShell's own (a function's unbound arguments), and this
        file now defines functions. Assigning to it is a trap set for the next
        helper someone adds."""
        self.assertNotRegex(PS, r"(?m)^\s*\$args\s*(=|\+=)")


if __name__ == "__main__":
    unittest.main()

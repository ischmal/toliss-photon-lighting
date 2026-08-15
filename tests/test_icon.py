"""The committed executable icon must still be the artwork it was generated from.

⚠ `src/native/res/photon.ico` IS A GENERATED FILE THAT LIVES IN `src/`. That is
deliberate — CMake has to build `photon-installer` on a runner with no Python, so
the .ico is a build INPUT there rather than something the build produces — and the
cost of it is drift: someone re-exports `src/icons/*.png`, never runs
`build/make_icon.py`, and the installer keeps shipping the previous artwork. The
failure is completely silent. The build succeeds, the binary is fine, and the only
symptom is an icon nobody looks at twice.

So this file re-derives the icon in memory and compares. It pins the pixels rather
than the file's bytes: the PNG-compressed entries go through zlib, and pinning
those would make the suite fail on a machine whose zlib compresses differently
while the artwork is perfectly in step.

⚠ IT ALSO PINS THE WIRING, not just the artwork. An .ico that no `.rc` names, or
an `.rc` that CMake never compiles, is exactly as blank as a stale one — and both
are one deleted line away at all times.
"""
import importlib.util
import pathlib
import unittest

REPO = pathlib.Path(__file__).resolve().parents[1]
NATIVE = REPO / "src" / "native"
ICO = NATIVE / "res" / "photon.ico"
RC = NATIVE / "res" / "photon.rc"
RESOURCE_H = NATIVE / "res" / "resource.h"
CMAKELISTS = NATIVE / "CMakeLists.txt"
WINDOW_ICON = NATIVE / "ui" / "assets" / "icon.png"
APP_SLINT = NATIVE / "ui" / "app.slint"
GUI_CPP = NATIVE / "src" / "installer" / "gui.cpp"


def _make_icon():
    """Import `build/make_icon.py` — not a package, so load it by path."""
    spec = importlib.util.spec_from_file_location(
        "make_icon", REPO / "build" / "make_icon.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


MAKE_ICON = _make_icon()


def _read_ico(data):
    """{size: payload} from an .ico, mirroring `make_icon.build_ico`'s output."""
    import struct

    reserved, kind, count = struct.unpack("<HHH", data[:6])
    assert reserved == 0 and kind == 1, "not an .ico"
    entries = {}
    for i in range(count):
        row = data[6 + i * 16:22 + i * 16]
        width = row[0] or 256      # 0 means 256; the field is one byte
        length, offset = struct.unpack("<II", row[8:16])
        entries[width] = data[offset:offset + length]
    return entries


class IconIsInStepWithTheArtwork(unittest.TestCase):
    def setUp(self):
        self.assertTrue(ICO.exists(), f"{ICO} is missing — run build/make_icon.py")
        self.sources = MAKE_ICON.load_sources()
        self.committed = _read_ico(ICO.read_bytes())

    def test_every_expected_size_is_present(self):
        self.assertEqual(sorted(self.committed),
                         sorted(MAKE_ICON.icon_sizes(self.sources)))

    def test_no_authored_size_is_dropped(self):
        """⚠ THE REGRESSION THIS EXISTS FOR (2026-08-14).

        `BASE_SIZES` used to BE the list rather than a floor, so an export at 60
        and 72 — Windows' large-icon sizes at 125% and 150% — sat in src/icons/
        matching no entry and never reached the .ico. Nothing warned: a dropped
        size looks exactly like a size the shell had to scale, which is the same
        thing this script exists to avoid.
        """
        missing = [s for s in sorted(self.sources)
                   if s <= MAKE_ICON.ICO_MAX and s not in self.committed]
        self.assertEqual(
            missing, [],
            f"src/icons/ has authored artwork at {missing}px that the .ico does "
            f"not carry — icon_sizes() must be the floor UNION the sources",
        )

    def test_the_committed_icon_matches_the_pngs(self):
        """The whole point: re-render from src/icons/ and compare every entry."""
        rendered = MAKE_ICON.render(self.sources)
        stale = []
        for size in sorted(rendered):
            if _pixels(rendered[size], size) != _pixels(self.committed[size], size):
                stale.append(size)
        self.assertEqual(
            stale, [],
            f"{ICO.name} no longer matches src/icons/ at {stale}px — "
            f"run `python build/make_icon.py` and commit the result",
        )

    def test_the_window_icon_matches_too(self):
        """`ui/assets/icon.png` is the second output of the same generator."""
        self.assertTrue(WINDOW_ICON.exists(), "run build/make_icon.py")
        want = MAKE_ICON.read_png(self.sources[MAKE_ICON.WINDOW_ICON][0])
        self.assertEqual(MAKE_ICON.read_png(WINDOW_ICON), want)

    def test_nothing_above_256_reaches_the_ico(self):
        """An icon directory entry has ONE BYTE per side, so 256 is the ceiling.

        ⚠ AND THE UNION RULE MAKES THIS REACHABLE. Sizes now come from the folder,
        so re-exporting at 512 — perfectly reasonable, and it is the best
        resampling source there is — would otherwise write 512 as its low byte: 0,
        i.e. a second entry claiming to be 256. Tested against a synthetic source
        set rather than today's folder, so it keeps holding on the day one appears.
        """
        self.assertLessEqual(max(self.committed), MAKE_ICON.ICO_MAX)
        pretend = dict.fromkeys([*self.sources, 512, 1024], None)
        sizes = MAKE_ICON.icon_sizes(pretend)
        self.assertNotIn(512, sizes)
        self.assertNotIn(1024, sizes)
        self.assertLessEqual(max(sizes), MAKE_ICON.ICO_MAX)


class TheIconIsActuallyWiredIn(unittest.TestCase):
    """A perfect .ico nothing compiles is indistinguishable from no .ico at all."""

    def test_the_rc_names_the_ico_by_the_shared_id(self):
        rc = RC.read_text(encoding="utf-8", errors="replace")
        self.assertRegex(
            rc, r"(?m)^\s*PHOTON_ICON_ID\s+ICON\s+\"photon\.ico\"",
            "photon.rc must declare the icon through the shared macro — gui.cpp "
            "LoadImage()s the same ID, and a number typed twice can disagree",
        )
        self.assertIn('#include "resource.h"', rc)
        header = RESOURCE_H.read_text(encoding="utf-8", errors="replace")
        self.assertRegex(
            header, r"(?m)^#define\s+PHOTON_ICON_ID\s+1\b",
            "the icon must stay resource ID 1 — the shell picks the LOWEST icon "
            "ID as the application icon",
        )

    def test_cmake_compiles_the_rc(self):
        """The .rc has to reach the installer target, and only there.

        Adding it to `photoncore` or `photoncore_tests` instead would compile
        cleanly and put the artwork nowhere a user sees it.
        """
        cmake = CMAKELISTS.read_text(encoding="utf-8", errors="replace")
        self.assertIn("PHOTON_INSTALLER_SOURCES res/photon.rc", cmake)


class TheWindowIconIsSplitByPlatform(unittest.TestCase):
    """⚠ THE REGRESSION THIS CLASS EXISTS FOR (2026-08-14).

    `Window.icon` is ONE image. Binding it to the 256 px artwork makes winit hand
    Windows a single 256x256 bitmap as the window's SMALL icon, and the title bar
    squashes it to 16 px with a crude stretch — while the hand-tuned 16 px entry
    in the .ico goes unused. It looks like bad artwork, not like bad wiring, and
    it survived a review because the taskbar and Explorer were always correct.

    So Windows sets the icon from the RESOURCE (ApplyWindowIcon → LoadImage at the
    exact size the shell asked for) and app.slint must leave the property EMPTY
    there. Both halves are pinned: re-adding the `@image-url` binding to `icon:`
    is the one edit that silently undoes all of it.
    """

    def setUp(self):
        self.slint = APP_SLINT.read_text(encoding="utf-8", errors="replace")
        self.gui = GUI_CPP.read_text(encoding="utf-8", errors="replace")

    def test_the_slint_icon_is_not_bound_to_an_image_url(self):
        self.assertNotRegex(
            self.slint, r"(?m)^\s*icon:\s*@image-url",
            "binding Window.icon directly gives Windows a 256px title-bar icon — "
            "bind it to the `window-icon` property and let C++ decide per platform",
        )

    def test_the_slint_icon_comes_from_a_property_cpp_can_fill(self):
        self.assertRegex(self.slint, r"(?m)^\s*in property <image> window-icon;")
        self.assertRegex(self.slint, r"(?m)^\s*icon:\s*root\.window-icon;")
        # The image still has to be EMBEDDED, or there is nothing for C++ to hand
        # back on Linux and no error to say so.
        self.assertRegex(
            self.slint,
            r'(?m)^\s*out property <image> icon-source: @image-url\("assets/icon\.png"\);',
        )

    def test_windows_uses_the_resource_and_everything_else_uses_slint(self):
        self.assertIn("ScheduleWindowIcon(kIconAttempts);", self.gui)
        self.assertIn("g.ui->set_window_icon(g.ui->get_icon_source());", self.gui)
        self.assertIn("#    include \"../../res/resource.h\"", self.gui)
        self.assertIn("MAKEINTRESOURCEW(PHOTON_ICON_ID)", self.gui)

    def test_the_icon_is_applied_from_inside_the_event_loop(self):
        """⚠ `run()` would hide the seam this fix lives in.

        There is no HWND until winit's first event-loop iteration, so the call has
        to sit between show() and run_event_loop(). Collapsing those back into
        `ui->run()` removes the only place the timer can be armed.
        """
        self.assertIn("g.ui->show();", self.gui)
        self.assertIn("slint::run_event_loop();", self.gui)
        self.assertNotIn("g.ui->run();", self.gui)

    def test_success_is_the_read_back_not_the_send(self):
        """The first fix reported a clean hit while the title bar stayed generic.

        It had painted a transient winit window and stopped retrying. Only the
        icon coming back out of WM_GETICON proves the right window was reached.
        """
        self.assertIn("WM_GETICON", self.gui)
        self.assertIn("found.confirmed > 0", self.gui)


def _pixels(payload, size):
    """Normalize an icon entry to RGBA pixels, whichever container it uses."""
    import struct

    if payload[:8] == b"\x89PNG\r\n\x1a\n":
        import io
        import tempfile

        with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as handle:
            handle.write(payload)
            path = pathlib.Path(handle.name)
        try:
            return MAKE_ICON.read_png(path)[2]
        finally:
            path.unlink()

    # BITMAPINFOHEADER: 32-bpp BGRA, bottom-up, with the AND mask after it.
    header = struct.unpack("<IiiHH", payload[:16])
    self_size, width, height, planes, bits = header
    assert self_size == 40 and bits == 32, "unexpected icon entry format"
    assert width == size and height == size * 2, "icon entry is the wrong size"
    out = bytearray(size * size * 4)
    for y in range(size):
        row = 40 + (size - 1 - y) * size * 4
        for x in range(size):
            s = row + x * 4
            d = (y * size + x) * 4
            out[d:d + 4] = bytes(
                (payload[s + 2], payload[s + 1], payload[s], payload[s + 3])
            )
    return out


if __name__ == "__main__":
    unittest.main()

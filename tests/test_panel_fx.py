"""The Panel FX compositor's cross-file contracts, checked against plugin.cpp.

Everything here fails SILENTLY in the simulator. A blend mode whose name grew a
space still compiles and still draws; it just cannot be read back out of the save
file, so the look reverts to Multiply the next time X-Plane starts. A tile value
that defaults to 0 instead of 1 samples one texel and looks like a flat colour.
Neither reads as "the file format is wrong" from the cockpit.
"""
import pathlib
import re
import struct
import unittest
import zlib

REPO = pathlib.Path(__file__).resolve().parents[1]
PLUGIN_CPP = (REPO / "src" / "native" / "src" / "plugin.cpp").read_text(
    encoding="utf-8", errors="replace"
)
OVERLAYS = REPO / "src" / "native" / "overlays"


def _string_array(name):
    """The string literals of a `static const char* <name>[N] = { ... };`."""
    start = PLUGIN_CPP.index("static const char* %s[" % name)
    end = PLUGIN_CPP.index("};", start)
    body = PLUGIN_CPP[start:end]
    body = body[body.index("{") + 1:]

    # Split on commas that are NOT inside a string literal — the help strings are
    # full of them. Adjacent literals concatenate in C, so each entry is then the
    # join of its own literals.
    entries, cur, in_str, escaped = [], "", False, False
    for ch in body:
        if escaped:
            escaped = False
        elif ch == "\\" and in_str:
            escaped = True
        elif ch == '"':
            in_str = not in_str
        elif ch == "," and not in_str:
            entries.append(cur)
            cur = ""
            continue
        cur += ch
    entries.append(cur)

    out = []
    for e in entries:
        parts = re.findall(r'"((?:[^"\\]|\\.)*)"', e)
        if parts:
            out.append("".join(parts))
    return out


def _blend_enum():
    start = PLUGIN_CPP.index("    kFxMultiply = 0,")
    end = PLUGIN_CPP.index("};", start)
    names = re.findall(r"\bk(Fx[A-Za-z]+)\b", PLUGIN_CPP[start:end])
    return [n for n in names if n != "FxBlendCount"]


class BlendModeTableTests(unittest.TestCase):
    def setUp(self):
        self.enum = _blend_enum()
        self.names = _string_array("kFxBlendName")
        self.helps = _string_array("kFxBlendHelp")

    def test_the_tables_parsed(self):
        self.assertGreaterEqual(len(self.enum), 8)

    def test_every_mode_has_a_name_and_a_help_string(self):
        # A short kFxBlendName is read past its end by the Combo, and a short
        # kFxBlendHelp by the tooltip — both out-of-bounds, neither diagnosed.
        self.assertEqual(len(self.names), len(self.enum))
        self.assertEqual(len(self.helps), len(self.enum))

    def test_blend_names_are_single_words(self):
        # DeserializeFxStacks reads the mode with `>>`, which stops at the first
        # space. A two-word mode would load back as Multiply, silently.
        for n in self.names:
            self.assertNotIn(" ", n, "blend mode %r must be one word" % n)
            self.assertTrue(n.strip(), "blend mode name is blank")

    def test_blend_names_are_unique(self):
        # FxBlendFromName returns the FIRST match, so a duplicate makes one mode
        # unreachable after a reload.
        self.assertEqual(sorted(self.names), sorted(set(self.names)))


class WireFormatTests(unittest.TestCase):
    """One serializer feeds the save file, undo, the journal and the clipboard."""

    def _layer_format(self):
        m = re.search(r'"layer %s %d ([^"]*)\\n"', PLUGIN_CPP)
        self.assertIsNotNone(m, "the layer line's format string moved")
        return m.group(1)

    def test_the_image_name_is_the_last_field(self):
        # File names contain spaces. Any field after the name would be
        # unparseable, because the reader takes the rest of the line as the name.
        fmt = self._layer_format()
        self.assertTrue(fmt.rstrip().endswith("%s"),
                        "the image name must be last on the layer line, got %r" % fmt)
        self.assertEqual(fmt.count("%s"), 1,
                         "a second string field would be ambiguous with the image name")

    def test_the_reader_takes_the_image_name_with_getline(self):
        # `in >> tex` would stop at the first space and leave the rest of the
        # name to be read as the next layer's blend mode.
        self.assertRegex(PLUGIN_CPP, r"std::getline\(in, tex\)")

    def test_tile_is_not_read_straight_into_the_layer(self):
        # ⚠ operator>> ZEROES its target on failure (C++11), so reading into
        # l.tile would turn every layer saved before images existed into tile 0 —
        # a single texel stretched over the target, which looks like a flat
        # colour and not like a defaulting bug.
        self.assertNotRegex(PLUGIN_CPP, r"in\s*>>\s*l\.tile")
        self.assertRegex(PLUGIN_CPP, r"if\s*\(in\s*>>\s*tile\)")

    def test_the_no_image_marker_is_written_and_read(self):
        # An empty tail is indistinguishable from a truncated line, so "none" has
        # to be a token. Both halves must agree on which one.
        self.assertRegex(PLUGIN_CPP, r'l\.tex\.empty\(\)\s*\?\s*"-"')
        self.assertRegex(PLUGIN_CPP, r'tex\s*!=\s*"-"')


class PerTargetSwitchTests(unittest.TestCase):
    """`follow` and `curve` as a tri-state on FxStack, not two more globals.

    Every failure here is silent in the same way: the tint still draws, it just
    dims (or does not dim) differently from what the pane says, and there is
    nothing in the cockpit that reads as "the switch did not stick".
    """

    def _fn(self, name):
        m = re.search(r"static [\w:]+ %s\(.*?\n\}" % name, PLUGIN_CPP, re.S)
        self.assertIsNotNone(m, "%s moved or was renamed" % name)
        return m.group(0)

    def test_inherit_is_negative_and_is_the_default(self):
        # A tri-state stored as a bool pair, or with inherit at 0, would make
        # "not set" indistinguishable from "explicitly off" — and off is the
        # NON-default here, so every existing stack would flip on load.
        self.assertRegex(PLUGIN_CPP,
                         r"enum \{ kFxInherit = -1, kFxOff = 0, kFxOn = 1 \};")
        self.assertRegex(PLUGIN_CPP, r"int\s+follow\s*=\s*kFxInherit;")
        self.assertRegex(PLUGIN_CPP, r"int\s+curve\s*=\s*kFxInherit;")

    def test_the_master_switch_still_outranks_a_target(self):
        # gFxFollowBrightness is the escape hatch you flip while picking a
        # colour. A per-target On that beat it would make the hatch look broken,
        # months after the target was set and with nothing on screen to connect
        # the two.
        self.assertRegex(self._fn("FxStackFollows"),
                         r"return\s+gFxFollowBrightness\s*&&\s*FxTriState")

    def test_follow_off_does_not_disable_the_power_gate(self):
        # The whole point of the setting: "full strength whenever this is
        # powered". Returning 1.0 before the gate would paint a tint over a dead
        # screen, which is the artefact the driver mechanism exists to remove.
        body = self._fn("FxRectFactor")
        gate = body.index("FxDriverPowered")
        follow = body.index("FxStackFollows")
        self.assertLess(gate, follow,
                        "the power gate must be evaluated before `follow`")

    def test_the_per_target_switches_have_their_own_wire_keys(self):
        # Parsing dispatches on the key alone. A per-stack line spelled "follow"
        # is the master switch to the reader, so loading a file would move a
        # target's setting onto the global one.
        self.assertIn('"tfollow %d\\n"', PLUGIN_CPP)
        self.assertIn('"tcurve %d\\n"', PLUGIN_CPP)
        self.assertRegex(PLUGIN_CPP, r'key == "tfollow" \|\| key == "tcurve"')

    def test_inherit_is_not_written_to_the_file(self):
        # Writing it would bake today's master value into every stack the first
        # time anything is saved, quietly ending the inheritance.
        self.assertRegex(PLUGIN_CPP, r"follow != kFxInherit")
        self.assertRegex(PLUGIN_CPP, r"curve != kFxInherit")

    def test_a_switch_before_any_target_is_ignored(self):
        # Same guard the driver lines carry: gFxStacks.back() on an empty vector
        # is undefined behaviour, and a hand-edited file is the normal way one
        # of these lines ends up first.
        m = re.search(r'key == "tfollow" \|\| key == "tcurve"\)\s*\{(.*?)\n\s*\}',
                      PLUGIN_CPP, re.S)
        self.assertIsNotNone(m)
        self.assertIn("gFxStacks.empty()", m.group(1))


class TextureCapabilityTests(unittest.TestCase):
    """Which blend modes can carry an image, and that the answer is used once."""

    def test_exactly_burn_and_darken_refuse_an_image(self):
        # Both are limits of fixed-function GL, not gaps: Burn emits the
        # COMPLEMENT of the colour and Darken is GL_MIN, and GL_MODULATE can
        # express neither per pixel. If this list ever grows, the editor's
        # warning and the docs have to grow with it.
        m = re.search(r"static bool FxBlendTakesTexture\(int blend\)\s*\{(.*?)\}",
                      PLUGIN_CPP, re.S)
        self.assertIsNotNone(m, "FxBlendTakesTexture moved or was renamed")
        refused = set(re.findall(r"blend\s*!=\s*k(Fx\w+)", m.group(1)))
        self.assertEqual(refused, {"FxBurn", "FxDarken"})

    def test_the_draw_path_honours_it(self):
        # A UI-only check would leave the compositor drawing a mode it cannot
        # express — the exact thing the function exists to prevent.
        draw = PLUGIN_CPP[PLUGIN_CPP.index("static void DrawFxLayer("):]
        draw = draw[:draw.index("\nstatic ")]
        self.assertIn("FxBlendTakesTexture(layer.blend)", draw)


class OverlayImageTests(unittest.TestCase):
    """The starter images, and that make_overlays.py still produces them."""

    def _decode(self, path):
        b = path.read_bytes()
        self.assertEqual(b[:8], b"\x89PNG\r\n\x1a\n", path.name)
        pos, idat, ihdr = 8, b"", None
        while pos < len(b):
            n = struct.unpack(">I", b[pos:pos + 4])[0]
            tag, data = b[pos + 4:pos + 8], b[pos + 8:pos + 8 + n]
            crc = struct.unpack(">I", b[pos + 8 + n:pos + 12 + n])[0]
            self.assertEqual(crc, zlib.crc32(tag + data) & 0xFFFFFFFF,
                             "%s: bad CRC on %s" % (path.name, tag))
            if tag == b"IHDR":
                ihdr = struct.unpack(">IIBBBBB", data)
            elif tag == b"IDAT":
                idat += data
            pos += 12 + n
        w, h, depth, ctype = ihdr[0], ihdr[1], ihdr[2], ihdr[3]
        # stb decodes far more than this, but the plugin asks for 4 channels and
        # premultiplies, so an image with no alpha channel would be a silent
        # full-coverage overlay.
        self.assertEqual((depth, ctype), (8, 6), "%s is not RGBA8" % path.name)
        raw = zlib.decompress(idat)
        stride = w * 4
        rows = [raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)] for y in range(h)]
        return w, h, (lambda x, y: rows[y][x * 4 + 3])

    def test_the_starter_overlays_exist(self):
        names = sorted(p.name for p in OVERLAYS.glob("*.png"))
        self.assertEqual(names, ["gradient_top.png", "scanlines.png",
                                 "sheen_diagonal.png", "vignette_edges.png"])

    def test_every_overlay_is_a_valid_rgba8_png(self):
        for p in sorted(OVERLAYS.glob("*.png")):
            self._decode(p)

    def test_the_vignette_is_opaque_at_the_EDGES(self):
        # ⚠ Inverted, this is not a subtle difference: paired with a Multiply
        # layer it would darken the middle of a readout and leave the frame
        # alone, which is the opposite of a vignette.
        w, h, a = self._decode(OVERLAYS / "vignette_edges.png")
        self.assertEqual(a(w // 2, h // 2), 0)
        self.assertEqual(a(0, 0), 255)
        self.assertEqual(a(w - 1, h - 1), 255)

    def test_the_top_gradient_is_opaque_at_the_top(self):
        w, h, a = self._decode(OVERLAYS / "gradient_top.png")
        self.assertEqual(a(w // 2, 0), 255)
        self.assertEqual(a(w // 2, h - 1), 0)

    def test_the_scanline_tile_alternates(self):
        # Small on purpose — the pitch is set by the layer's Tile value, so a
        # tile that is accidentally uniform reads as "tiling does not work".
        w, h, a = self._decode(OVERLAYS / "scanlines.png")
        col = [a(0, y) for y in range(h)]
        self.assertIn(0, col)
        self.assertIn(255, col)

    def test_the_generator_reproduces_them_byte_for_byte(self):
        # They are committed so a checkout can deploy without running Python,
        # which only stays true while the two agree.
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "make_overlays", REPO / "build" / "make_overlays.py")
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        for name, (w, h, fn) in sorted(mod.IMAGES.items()):
            tmp = OVERLAYS / (name + ".check")
            try:
                mod.write_png(tmp, w, h, mod.build(w, h, fn))
                self.assertEqual(tmp.read_bytes(), (OVERLAYS / name).read_bytes(),
                                 "%s is stale - re-run build/make_overlays.py" % name)
            finally:
                if tmp.exists():
                    tmp.unlink()


class DeployTests(unittest.TestCase):
    def test_deploy_copies_the_overlay_folder(self):
        # The plugin looks beside its own .xpl for overlays/. A deploy that does
        # not put them there leaves an empty picker and no clue why.
        ps1 = (REPO / "src" / "native" / "deploy.ps1").read_text(
            encoding="utf-8", errors="replace")
        self.assertIn("overlays", ps1)

    def test_deploy_does_not_mirror_the_folder(self):
        # The destination is also where the user drops their OWN images. A
        # recursive -Force copy of the whole tree is one flag away from removing
        # them, so the copy is per-file by name.
        ps1 = (REPO / "src" / "native" / "deploy.ps1").read_text(
            encoding="utf-8", errors="replace")
        self.assertNotRegex(ps1, r"Copy-Item[^\r\n]*overlays[^\r\n]*-Recurse")
        self.assertNotRegex(ps1, r"Remove-Item[^\r\n]*overlay")


if __name__ == "__main__":
    unittest.main()

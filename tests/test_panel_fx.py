"""The Panel FX compositor's cross-file contracts, checked against plugin.cpp.

Everything here fails SILENTLY in the simulator. A blend mode whose name grew a
space still compiles and still draws; it just cannot be read back out of the save
file, so the look reverts to Multiply the next time X-Plane starts. A tile value
that defaults to 0 instead of 1 samples one texel and looks like a flat color.
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
PANELFX = REPO / "src" / "native" / "panelfx.txt"


def _generated_overlay_names():
    """The overlay names build/make_overlays.py produces, read from the script so
    the two cannot drift. Imported lazily and by path: build/ is not a package."""
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "photon_make_overlays", REPO / "build" / "make_overlays.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return sorted(mod.IMAGES)


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


def _dev_guarded_at(idx):
    """Whether byte `idx` of plugin.cpp sits inside a `#if PHOTON_DEV` block, by
    counting guard opens and closes ahead of it. Crude, and that is the point: it
    reads the file the same way the preprocessor does rather than trusting a
    comment."""
    depth = 0
    for line in PLUGIN_CPP[:idx].splitlines():
        s = line.strip()
        if s.startswith("#if PHOTON_DEV"):
            depth += 1
        elif s.startswith("#endif") and depth:
            depth -= 1
        elif s.startswith("#if") or s.startswith("#ifdef") or s.startswith("#ifndef"):
            # A nested, unrelated #if inside the guard — its #endif would
            # otherwise be counted as closing ours.
            if depth:
                depth += 1
    return depth > 0


def _dev_guarded(symbol):
    return _dev_guarded_at(PLUGIN_CPP.index(symbol))


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
        # color and not like a defaulting bug.
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
        # color. A per-target On that beat it would make the hatch look broken,
        # months after the target was set and with nothing on screen to connect
        # the two.
        self.assertRegex(self._fn("FxStackFollows"),
                         r"return\s+gFxFollowBrightness\s*&&\s*FxTriState")

    def test_follow_off_does_not_disable_the_power_gate(self):
        # The whole point of the setting: "full strength whenever this is
        # powered". Returning 1.0 before the gate would paint a tint over a dead
        # screen, which is the artifact the driver mechanism exists to remove.
        #
        # ⚠ `follow` now resolves at the LAYER level (FxLayerFollows), so this
        # reads the layer-level call. The rule is the same one and applies to
        # both: every gate is evaluated before anything asks whether to dim.
        body = self._fn("FxRectFactor")
        gate = body.index("FxDriverPowered")
        follow = body.index("FxLayerFollows")
        self.assertLess(gate, follow,
                        "the power gate must be evaluated before `follow`")

    def test_a_layers_follow_inherits_from_its_stack_not_from_the_master(self):
        # Three levels — master, target, layer — and each Inherit resolves to the
        # NEXT ONE OUT, not straight to the global. A layer reading
        # gFxFollowBrightness for its inherited value would ignore its target's
        # setting entirely, so a target switched to "steady" would still have
        # every one of its layers dimming.
        body = self._fn("FxLayerFollows")
        self.assertIn("FxStackFollows(s)", body,
                      "a layer's inherited value is its STACK's setting")
        # And the master still outranks both, for the same reason it outranks a
        # target: it is a tuning hatch, and a layer set months ago must not be
        # the reason it looks broken today.
        self.assertRegex(body, r"gFxFollowBrightness\s*\n?\s*&&")

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
        # is undefined behavior, and a hand-edited file is the normal way one
        # of these lines ends up first.
        m = re.search(r'key == "tfollow" \|\| key == "tcurve"\)\s*\{(.*?)\n\s*\}',
                      PLUGIN_CPP, re.S)
        self.assertIsNotNone(m)
        self.assertIn("gFxStacks.empty()", m.group(1))


class TextureCapabilityTests(unittest.TestCase):
    """Which blend modes can carry an image, and that the answer is used once."""

    def test_exactly_burn_and_darken_refuse_an_image(self):
        # Both are limits of fixed-function GL, not gaps: Burn emits the
        # COMPLEMENT of the color and Darken is GL_MIN, and GL_MODULATE can
        # express neither per pixel. If this list ever grows, the editor's
        # warning and the docs have to grow with it.
        m = re.search(r"static bool FxBlendTakesTexture\(int blend\)\s*\{(.*?)\}",
                      PLUGIN_CPP, re.S)
        self.assertIsNotNone(m, "FxBlendTakesTexture moved or was renamed")
        refused = set(re.findall(r"blend\s*!=\s*k(Fx\w+)", m.group(1)))
        self.assertEqual(refused, {"FxBurn", "FxDarken"})

    def test_the_draw_path_honors_it(self):
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

    def test_the_generated_starter_overlays_exist(self):
        # A SUBSET check, not equality. The folder is no longer only
        # make_overlays.py's output: it is also where hand-painted images live
        # (screen-bleed.png, screen-lcd.png — which panelfx.txt names and no
        # script can reproduce), and it is what the installer now ships. Equality
        # here would fail the moment the look gained an image, which is a normal
        # thing for the look to do.
        names = {p.name for p in OVERLAYS.glob("*.png")}
        missing = sorted(set(_generated_overlay_names()) - names)
        self.assertEqual(missing, [], "make_overlays.py output missing from "
                                      "src/native/overlays/")

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


class AmbientBlendTests(unittest.TestCase):
    """The day/night blend. Every failure here is a look that is simply wrong at
    one end of the day, on a machine where it was authored at the other."""

    def _fn(self, name):
        # ⚠ The signature line must END IN `{`, or a forward declaration
        # ("static void FxForgetDrivers();") matches and what comes back is the
        # body of whatever function happens to follow it.
        m = re.search(r"^static [^\n]*\b%s\([^;\n]*\)[^\n]*\{\n(?:.*?\n)*?\}"
                      % re.escape(name), PLUGIN_CPP, re.M)
        self.assertIsNotNone(m, "%s not found" % name)
        return m.group(0)

    def test_the_source_is_the_one_tolil_reads(self):
        # Not an arbitrary "is it daytime" dataref. cockpit_light_level_* is what
        # the AirbusFBW binary reads, so Photon and the aircraft agree about dusk
        # by construction rather than by coincidence.
        for ch in "rgb":
            self.assertIn("sim/graphics/misc/cockpit_light_level_%s" % ch, PLUGIN_CPP)

    def test_a_layer_with_no_day_half_is_returned_untouched(self):
        # THE BACKWARD-COMPATIBILITY GUARANTEE. Every layer in the shipped
        # panelfx.txt predates the blend; if the lerp ran on their default
        # dayColor/dayOpacity the whole look would fade toward white and vanish
        # by mid-morning, in a release, with nothing in the log.
        body = self._fn("FxLayerAtAmbient")
        self.assertRegex(body, r"if\s*\(!l\.hasDay\)\s*return n;")
        self.assertLess(body.index("hasDay"), body.index("FxAmbientNow"),
                        "the hasDay test must come before anything reads ambient")

    def test_the_quad_colour_cannot_be_taken_from_the_layer(self):
        # ⚠ FxQuadColorFor takes the COLOUR, not the layer, so there is no
        # `layer.color` in scope for it to reach for. Passing the layer would
        # compile and would draw the night colour at every hour, with only the
        # opacity following the sun — a half-working blend is far harder to spot
        # than one that does nothing.
        m = re.search(r"static FxQuadColor FxQuadColorFor\(([^)]*)\)", PLUGIN_CPP)
        self.assertIsNotNone(m)
        self.assertNotIn("FxLayer", m.group(1),
                         "FxQuadColorFor must not take an FxLayer")
        self.assertIn("const float color[3]", m.group(1))

    def test_the_draw_test_uses_the_blended_opacity(self):
        # A daytime-only layer has an authored opacity of 0. Skipping on that
        # value means it never draws — the day half silently does not work, and
        # only in the one configuration anybody would use it for.
        body = self._fn("DrawFxLayer")
        self.assertRegex(body, r"const FxLayerNow now = FxLayerAtAmbient\(layer\);")
        self.assertRegex(body, r"if\s*\(now\.opacity <= 0\.0f\)\s*return;")
        # And the caller must NOT re-test the authored one.
        loop = re.search(r"for \(size_t i = 0; i < stack\.layers\.size\(\); \+\+i\) \{"
                         r"(?:.*?\n)*?\s*\}", PLUGIN_CPP)
        self.assertIsNotNone(loop)
        self.assertNotIn("layer.opacity > 0.0f", loop.group(0))

    def test_the_blend_is_smoothed_against_the_wall_clock(self):
        # A per-frame constant would run the fade at a different speed on every
        # machine, and a jump-cut on the raw value makes every screen shift
        # colour the moment a cloud crosses the sun.
        body = self._fn("FxSampleAmbient")
        self.assertIn("XPLMGetElapsedTime", body)
        self.assertIn("std::exp(-dt / gFxAmbientTau)", body)

    def test_the_first_sample_snaps(self):
        # Otherwise every flight starting in daylight fades up from "night" over
        # the first seconds, which reads as the effect booting rather than as a
        # cockpit.
        body = self._fn("FxSampleAmbient")
        self.assertIn("const bool  first = gFxAmbientClock < 0.0;", body)
        self.assertRegex(body, r"if \(first \|\|[^\n]*\)\s*\{\s*\n\s*gFxAmbient = target;")

    def test_an_unreadable_source_holds_its_value(self):
        # Same house rule as the drivers: collapsing to 0 would repaint the whole
        # cockpit in its night look because one dataref went missing.
        self.assertRegex(self._fn("FxAmbientRawNow"),
                         r"return best < 0\.0f \? gFxAmbientRaw : best;")


class PerLayerWireTests(unittest.TestCase):
    """`day` and `lfollow` are their own lines, and that is not a style choice."""

    def test_the_layer_extras_are_separate_lines(self):
        # ⚠ The layer line ENDS with an image name read by getline, precisely
        # because names contain spaces. Nothing can ever follow it on that line,
        # so both extras are lines of their own — the same shape as tfollow.
        self.assertIn('"day %.4f %.4f %.4f %.4f\\n"', PLUGIN_CPP)
        self.assertIn('"lfollow %d\\n"', PLUGIN_CPP)
        layer_line = re.search(r'"layer %s %d[^"]*"', PLUGIN_CPP).group(0)
        self.assertTrue(layer_line.rstrip('"').endswith("%s\\n"),
                        "the image name must stay last on the layer line")

    def test_they_attach_to_the_last_layer_not_the_last_target(self):
        # The key list grows as layers gain extras (day, lfollow, lcurve); what
        # this pins is that ALL of them resolve to the last LAYER and that a
        # malformed file cannot walk off the end of either vector.
        m = re.search(r'key == "day" \|\|(?:[^)]*)\)\s*\{(.*?)\n\s*\} else',
                      PLUGIN_CPP, re.S)
        self.assertIsNotNone(m)
        self.assertIn("gFxStacks.back().layers.back()", m.group(1))
        # And a malformed file must not walk off the end of either vector.
        self.assertIn("gFxStacks.back().layers.empty()", m.group(1))

    def test_unset_extras_are_not_written(self):
        # A file whose layers are all era-agnostic and all inherit has to stay
        # byte-identical to what the previous version wrote, or every upgrade
        # produces a diff nobody can review.
        self.assertRegex(PLUGIN_CPP, r"if \(l\.hasDay\) \{")
        self.assertRegex(PLUGIN_CPP, r"if \(l\.follow != kFxInherit\) \{")

    def test_the_shipped_definition_still_parses_as_the_old_format(self):
        # panelfx.txt is authored by the dev build and shipped to every user. It
        # must not have grown a key the loader does not know.
        known = {"enabled", "follow", "curve", "ambient", "ambientref", "target",
                 "bright", "power", "tfollow", "tcurve", "layer",
                 "day", "lfollow", "lcurve"}
        for line in PANELFX.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            self.assertIn(line.split()[0], known,
                          "panelfx.txt line the loader would drop: %r" % line)


class LayerCurveTests(unittest.TestCase):
    """A LAYER may carry its own response curve (docs/fcu_tint_plan.md §8i).

    Three ways this goes wrong quietly. It could be read as a property of the
    target, which puts two layers that need opposite shapes back on one setting.
    It could be applied without outranking the target's response switch, so a
    curve you drew and can see plotted is not the curve being drawn. And it could
    leak into the spill lights, which are shipping and unswitchable.
    """

    def test_the_curve_lives_on_the_layer_not_on_the_stack(self):
        layer = re.search(r"struct FxLayer \{.*?\n\};", PLUGIN_CPP, re.S).group(0)
        self.assertIn("FxCurve curve;", layer)
        stack = re.search(r"struct FxStack \{.*?\n\};", PLUGIN_CPP, re.S).group(0)
        self.assertNotIn("FxCurve ", stack,
                         "a target's shape is its tri-state `curve`, not knots")

    def test_a_layers_knots_outrank_the_targets_response_switch(self):
        """Including \"Linear\". The editor plots the curve it is editing, so
        anything that could override it would make that plot a lie."""
        m = re.search(r"static const FxCurve\* FxShapeFor\(.*?\n\}", PLUGIN_CPP, re.S)
        self.assertIsNotNone(m, "FxShapeFor moved")
        body = m.group(0)
        layer_line = body.index("l->curve.on")
        stack_line = body.index("FxShapeForStack")
        self.assertLess(layer_line, stack_line,
                        "the layer's own curve must be tested first")
        # And a half-built curve is not a curve: fewer than two knots falls back
        # rather than evaluating an identity nobody asked for.
        self.assertIn("knots.size() >= 2", body)

    def test_the_compositor_asks_for_the_layers_shape(self):
        """FxRectFactor is per rect AND per layer — gFxDrawLayer is set for
        exactly this. Passing the stack's shape here compiles and silently
        ignores every authored curve."""
        m = re.search(r"static float FxRectFactor\(int rectIndex,.*?\n\}",
                      PLUGIN_CPP, re.S)
        self.assertIsNotNone(m, "FxRectFactor moved")
        self.assertIn("FxShapeFor(gFxDrawLayer, gFxDrawStack)", m.group(0))

    def test_the_wire_line_carries_its_own_count(self):
        """⚠ A knot per line would let any edit that loses a line turn a shape
        into a DIFFERENT, perfectly valid shape with nothing to mark the loss.
        The count is what lets the reader refuse a truncated table outright."""
        self.assertIn('"lcurve %d"', PLUGIN_CPP)
        m = re.search(r'\} else if \(key == "lcurve"\) \{(.*?)\n            \} else',
                      PLUGIN_CPP, re.S)
        self.assertIsNotNone(m, "the lcurve reader moved")
        body = m.group(1)
        self.assertIn("kFxCurveMaxKnots", body, "the count must be bounded")
        # Assembled in a local and adopted whole — never straight into the layer.
        self.assertIn("FxCurve c;", body)
        self.assertIn("l.curve = c;", body)

    def test_an_unset_curve_is_not_written(self):
        """A file whose layers all inherit has to stay byte-identical to what the
        previous version wrote, or every upgrade produces an unreviewable diff."""
        self.assertRegex(PLUGIN_CPP,
                         r"if \(l\.curve\.on && l\.curve\.knots\.size\(\) >= 2\) \{")

    def _shipped_curves(self):
        """Every `lcurve` line in the shipped look, as (count, [(x, y), ...]).

        The FCU row's Lighten layer carries one as of 2026-08-04 — the first
        authored curve to ship. Until then this file had none and a test simply
        said so; that tripwire has done its job and these replace it.
        """
        out = []
        for raw in PANELFX.read_text(encoding="utf-8").splitlines():
            line = raw.strip()
            if not line.startswith("lcurve "):
                continue
            f = line.split()[1:]
            count = int(f[0])
            nums = [float(v) for v in f[1:]]
            out.append((count, list(zip(nums[0::2], nums[1::2]))))
        return out

    def test_a_shipped_curves_count_matches_the_pairs_that_follow(self):
        """⚠ The reader FALLS BACK on a mismatch — the layer quietly keeps the
        target's response curve, with one line in Log.txt nobody is reading. Now
        that a curve ships, a miscounted line is a shipped layer silently losing
        its shape, which is exactly what the count field exists to prevent and
        cannot prevent in the file it is written into."""
        for count, knots in self._shipped_curves():
            self.assertEqual(count, len(knots),
                             "lcurve says %d knots and carries %d"
                             % (count, len(knots)))

    def test_a_shipped_curve_is_one_the_reader_will_accept(self):
        """2..kFxCurveMaxKnots, or the reader ignores the line outright."""
        m = re.search(r"kFxCurveMaxKnots\s*=\s*(\d+)", PLUGIN_CPP)
        self.assertIsNotNone(m, "kFxCurveMaxKnots moved")
        cap = int(m.group(1))
        for count, _ in self._shipped_curves():
            self.assertGreaterEqual(count, 2, "under two knots is not a curve")
            self.assertLessEqual(count, cap, "over kFxCurveMaxKnots is dropped")

    def test_a_shipped_curve_survives_normalization_unchanged(self):
        """⚠ FxCurveNormalize clamps to 0..1, sorts by x and PINS THE ENDS to
        x 0 and 1. A file whose curve is not already in that form loads as a
        different shape than it reads as — and the next save in the editor
        rewrites the shipped default with no edit having been made, which is an
        unreviewable diff arriving on its own."""
        for count, knots in self._shipped_curves():
            xs = [x for x, _ in knots]
            self.assertEqual(xs, sorted(xs), "knots are not in ascending x")
            self.assertAlmostEqual(xs[0], 0.0, places=4,
                                   msg="the first knot is not pinned to x = 0")
            self.assertAlmostEqual(xs[-1], 1.0, places=4,
                                   msg="the last knot is not pinned to x = 1")
            for x, y in knots:
                self.assertGreaterEqual(x, 0.0)
                self.assertLessEqual(x, 1.0)
                self.assertGreaterEqual(y, 0.0, "y clamps to 0..1 on load")
                self.assertLessEqual(y, 1.0, "y clamps to 0..1 on load")

    def test_the_domain_is_forced_but_the_range_is_not(self):
        """A curve that starts at x = 0.3 would silently hold its own first y
        across the bottom third of every knob, so the ends are pinned. y is NOT
        forced upward: a wash that peaks mid-travel is a real authored look, and
        BuildMonotoneTangents zeroes the tangent at the turn."""
        m = re.search(r"static void FxCurveNormalize\(.*?\n\}", PLUGIN_CPP, re.S)
        self.assertIsNotNone(m)
        body = m.group(0)
        self.assertIn("c.knots.front().x = 0.0f;", body)
        self.assertIn("c.knots.back().x  = 1.0f;", body)
        tangents = re.search(r"static void BuildMonotoneTangents\(.*?\n\}",
                             PLUGIN_CPP, re.S).group(0)
        self.assertIn("secant[i - 1] * secant[i] < 0.0f", tangents,
                      "a knot the curve turns around at needs a zero tangent")

    def test_the_editor_is_dev_only_and_the_evaluation_is_not(self):
        guarded = ShippingBuildTests()._guarded
        self.assertTrue(guarded("static bool FxCurveEditor("),
                        "the curve editor is tuning, not part of the look")
        for symbol in ("static float FxCurveEval(",
                       "static void FxCurveNormalize(",
                       "static const FxCurve* FxShapeFor("):
            self.assertFalse(guarded(symbol),
                             "%s must ship — panelfx.txt can carry an lcurve" % symbol)


class ColourRampTests(unittest.TestCase):
    """A layer may carry a START colour for a dark knob (§8k).

    Every way this goes wrong is a look that is right at one end of the knob, so
    the cockpit reports it as "the ramp does nothing" whichever half is broken.
    """

    def test_the_ramp_lives_on_the_layer(self):
        layer = re.search(r"struct FxLayer \{.*?\n\};", PLUGIN_CPP, re.S).group(0)
        self.assertIn("bool  hasRamp", layer)
        self.assertIn("float rampColor[3]", layer)
        # ⚠ THREE floats. A fourth (an opacity of its own) would multiply into
        # the same number `lfollow` and the curve already produce, with no way to
        # tell from the cockpit which of the three dimmed a layer.
        stack = re.search(r"struct FxStack \{.*?\n\};", PLUGIN_CPP, re.S).group(0)
        self.assertNotIn("rampColor", stack,
                         "a ramp is a property of one painted layer")

    def test_a_layer_with_no_ramp_is_returned_untouched(self):
        """⚠ The backward-compatibility guarantee, same shape as the day half's.
        Every layer in the shipped panelfx.txt predates this, and interpolating
        them toward the struct's default white would wash the whole look out at
        any knob below full — in a release."""
        m = re.search(r"static void FxLayerColorAtDrive\(.*?\n\}", PLUGIN_CPP, re.S)
        self.assertIsNotNone(m, "FxLayerColorAtDrive moved")
        body = m.group(0)
        self.assertIn("if (!l.hasRamp)", body)
        self.assertLess(body.index("if (!l.hasRamp)"), body.index("out[i] = l.rampColor"),
                        "the early return must come before any interpolation")

    def test_the_ramp_interpolates_toward_the_ambient_resolved_colour(self):
        """⚠ It takes a COLOUR, not an FxLayer — the same signature rule
        FxQuadColorFor has and for the same reason. Reaching into `l.color` here
        would compile and would pin the top of every ramp to the NIGHT colour, so
        the day half would silently stop moving on any layer with a ramp."""
        m = re.search(r"static void FxLayerColorAtDrive\((.*?)\)\s*\{", PLUGIN_CPP, re.S)
        self.assertIsNotNone(m)
        self.assertIn("const float base[3]", m.group(1))
        # And the compositor hands it the day-blended colour, never layer.color.
        # ⚠ Anchored on the DEFINITION. There is a forward declaration of the
        # same signature above the probe, and a lazier pattern matches that and
        # then asserts happily about an empty body.
        m = re.search(r"static void DrawFxRect\([^;]*?\) \{.*?\n\}", PLUGIN_CPP, re.S)
        self.assertIsNotNone(m, "DrawFxRect's definition moved")
        draw = m.group(0)
        self.assertIn("FxLayerColorAtDrive(*gFxDrawLayer, gFxDrawNow.color", draw)

    def test_a_ramped_layer_skips_the_full_brightness_fast_path(self):
        """⚠ The fast path used to be `f >= 0.999`, which is TRUE for a layer
        that does not dim — and a colour correction that stays at full strength
        while powered is the layer most likely to carry a ramp. Taking it there
        draws the full-knob colour at every knob position: the feature doing
        nothing in exactly its own primary case."""
        # ⚠ Anchored on the DEFINITION. There is a forward declaration of the
        # same signature above the probe, and a lazier pattern matches that and
        # then asserts happily about an empty body.
        m = re.search(r"static void DrawFxRect\([^;]*?\) \{.*?\n\}", PLUGIN_CPP, re.S)
        self.assertIsNotNone(m, "DrawFxRect's definition moved")
        draw = m.group(0)
        self.assertIn("hasRamp", draw)
        m = re.search(r"if \(!ramped && f >= 0\.999f\)", draw)
        self.assertIsNotNone(m, "the fast path must test the ramp as well as f")

    def test_the_ramp_is_not_gated_on_follow(self):
        """`follow` answers whether the OPACITY tracks the knob; the ramp answers
        whether the COLOUR does. Tying them together would make a ramp
        unavailable on a colour correction, which is what wants one."""
        m = re.search(r"static float FxRectFactor\(int rectIndex,.*?\n\}",
                      PLUGIN_CPP, re.S).group(0)
        self.assertRegex(m, r"wantsDrive\s*=\s*follows \|\|.*hasRamp")
        # ...and the value is still read only when one of the two wants it, so a
        # plain layer pays nothing for the feature.
        self.assertIn("if (wantsDrive && bright && bright->set())", m)

    def test_the_wire_line_is_its_own_line_and_read_into_a_local(self):
        """The layer line ends with an image NAME read by getline, so nothing can
        follow it there. And ⚠ operator>> zeroes its target on failure: a short
        line read straight into the layer leaves a BLACK start colour with
        hasRamp true — a layer that fades to black at a dim knob, which is a
        plausible enough look to survive review."""
        self.assertIn('"ramp %.4f %.4f %.4f\\n"', PLUGIN_CPP)
        m = re.search(r'\} else if \(key == "ramp"\) \{(.*?)\n            \} else',
                      PLUGIN_CPP, re.S)
        self.assertIsNotNone(m, "the ramp reader moved")
        body = m.group(1)
        self.assertNotRegex(body, r"in\s*>>\s*l\.rampColor")
        self.assertIn("if (in >> c[0] >> c[1] >> c[2])", body)
        self.assertIn("l.hasRamp = true;", body)

    def test_an_unset_ramp_is_not_written(self):
        """A file whose layers have no ramp stays byte-identical to what the
        previous version wrote, or every upgrade is an unreviewable diff."""
        ser = re.search(r"static std::string SerializeFxStacks\(\).*?\n\}",
                        PLUGIN_CPP, re.S).group(0)
        self.assertIn("if (l.hasRamp) {", ser)

    def test_the_shipped_definition_carries_no_ramp_yet(self):
        """Nothing has been authored with one. When the first does, this becomes
        the place to check it is three parseable floats — the reader FALLS BACK
        on a bad line and logs one sentence nobody reads."""
        for raw in PANELFX.read_text(encoding="utf-8").splitlines():
            line = raw.strip()
            if line.startswith("#") or not line.startswith("ramp "):
                continue
            self.assertEqual(len(line.split()), 4,
                             "a shipped ramp line is `ramp <r> <g> <b>`: %r" % line)


class BrightnessPinTests(unittest.TestCase):
    """The dev-only held pin on every brightness driver (§8l)."""

    def test_the_control_is_dev_only_and_the_read_is_not(self):
        """gFxDriveManual is read from FxDriverNorm, which ships — exactly like
        gFxAmbientManual. It is -1 for the whole life of a release because
        nothing outside the guard can write it, which is what makes one compare
        the entire cost."""
        self.assertIn("static float gFxDriveManual = -1.0f;", PLUGIN_CPP)
        self.assertFalse(_dev_guarded("static float gFxDriveManual"),
                         "the value must be declared outside the dev guard, or "
                         "the shipping build will not compile FxDriverNorm")
        self.assertFalse(_dev_guarded("static bool FxDriverNorm("))

    def test_only_dev_code_ever_writes_it(self):
        """Every assignment is inside the guard. One outside would be a release
        that can pin its own brightness drivers with no way to unpin them."""
        writes, off = 0, 0
        for line in PLUGIN_CPP.splitlines(keepends=True):
            if (re.search(r"gFxDriveManual\s*=[^=]", line)
                    and "static float gFxDriveManual" not in line):
                writes += 1
                self.assertTrue(_dev_guarded_at(off),
                                "a write to gFxDriveManual outside #if "
                                "PHOTON_DEV: %r" % line.strip())
            off += len(line)
        self.assertGreaterEqual(writes, 2, "expected the hold and the release")
        for fn in ("static void FxHoldDriveOverride(", "static void FxReleaseDriveOverride("):
            self.assertIn(fn, PLUGIN_CPP)

    def test_the_pin_is_never_persisted(self):
        """It is a gesture, not a setting. Saving it would ship a look authored
        against a knob position nobody is holding."""
        save = re.search(r"static void SavePanelFx\(\).*?\n\}", PLUGIN_CPP, re.S).group(0)
        self.assertNotIn("gFxDriveManual", save)
        ser = re.search(r"static std::string SerializeFxStacks\(\).*?\n\}",
                        PLUGIN_CPP, re.S).group(0)
        self.assertNotIn("gFxDriveManual", ser)

    def test_it_pins_the_read_AND_writes_the_dataref(self):
        """Both halves, and neither is redundant. The read pin makes the tint
        sweep on a face whose source is read-only or unbound; the write makes
        ToLiss dim the display underneath, without which the tint is being
        judged against the wrong background — which is the mistake the whole
        driver mechanism exists to prevent."""
        # The read half stays in the driver's normalization.
        norm = re.search(r"static bool FxDriverNorm\(FxDriver& d, float\* out\)\s*\{(.*?)\n\}",
                         PLUGIN_CPP, re.S)
        self.assertIn("gFxDriveManual", norm.group(1))
        # The write half is its own function, and it is dev-only.
        self.assertIn("static bool FxWriteDriverValue(", PLUGIN_CPP)
        self.assertTrue(_dev_guarded("static bool FxWriteDriverValue("),
                        "a shipping build must never write ToLiss's datarefs")
        self.assertTrue(_dev_guarded("static void FxDriveTheAircraft("))

    def test_the_write_goes_back_through_lo_hi_and_not_through_the_curve(self):
        """⚠ Two different unit mistakes, both silent. Writing the normalized
        0..1 puts 0.4 into a knob that counts 0..100 and reads as the write not
        working. Writing the CURVED value bends the aircraft's own dimming
        through Photon's model of it and then reads it back through that model a
        second time."""
        m = re.search(r"static bool FxWriteDriverValue\(.*?\n\}", PLUGIN_CPP, re.S)
        self.assertIsNotNone(m, "FxWriteDriverValue moved")
        body = m.group(0)
        self.assertIn("d.lo + (double)t * ((double)d.hi - (double)d.lo)", body)
        self.assertNotIn("FxCurveEval", body)
        self.assertNotIn("FxDriverFactor", body)

    def test_the_write_asks_permission_and_probes_the_array_length(self):
        """⚠ XPLMCanWriteDataRef is the whole read-only story — without it the
        refusals are invisible and "that screen did not dim" has no answer. And a
        write past the end of ToLiss's array is undefined; the read at least only
        returned nonsense."""
        m = re.search(r"static bool FxWriteDriverValue\(.*?\n\}", PLUGIN_CPP, re.S)
        body = m.group(0)
        self.assertIn("XPLMCanWriteDataRef(d.ref)", body)
        self.assertEqual(body.count("nullptr, 0, 0) <"), 4,
                         "every array write needs its own length probe")
        # ⚠ Widest type first, mirroring FxReadScalar: writing a 0..1 knob
        # through an int writer quantizes it to 0 or 1, and the screen snapping
        # between off and full reads as the dataref not meaning what it says.
        self.assertLess(body.index("XPLMSetDataf"), body.index("XPLMSetDatai"))

    def test_the_writes_are_deduped_on_the_resolved_dataref(self):
        """Six DUs on one DUBrightness array are six FxDriver objects, and the
        six rects of a group are six more. Keying on the object would write the
        same element a dozen times a frame."""
        m = re.search(r"static void FxDriveTheAircraft\(.*?\n\}", PLUGIN_CPP, re.S)
        self.assertIsNotNone(m, "FxDriveTheAircraft moved")
        body = m.group(0)
        self.assertIn("seenRef[i] == d.ref && seenIdx[i] == d.index", body)
        # Every rect's driver AND every stack override — a stack override is the
        # dataref its target's faces actually read.
        self.assertIn("drive(gRectBright[i])", body)
        self.assertIn("drive(gFxStacks[s].bright)", body)


class ShippingBuildTests(unittest.TestCase):
    """The compositor SHIPS; the probe and the editor do not.

    Getting this wrong is silent in both directions. A compositor accidentally
    left inside `#if PHOTON_DEV` builds and runs fine on the dev machine and does
    nothing at all in a release. A probe accidentally left OUT of the guard paints
    magenta over the captain's PFD in a release."""

    def _guarded(self, symbol):
        return _dev_guarded(symbol)

    def test_the_compositor_is_compiled_into_the_shipping_build(self):
        for symbol in ("static void DrawPanelFx()",
                       "static void DrawFxLayer(",
                       "static float FxRectFactor(",
                       "static void LoadPanelFx()",
                       "static bool UploadFxTexture("):
            self.assertFalse(self._guarded(symbol),
                             "%s must not be behind #if PHOTON_DEV" % symbol)

    def test_the_probe_and_the_editor_stay_dev_only(self):
        for symbol in ("static void DrawProbeRect(",
                       "static void ClearProbeRect(",
                       "static void PushForcedState()",
                       "static void SavePanelFx()",
                       "static void BuildFxTab()",
                       # The CB index finder and the electrical readout are
                       # tuning instruments, not part of the look.
                       "static void BuildCbWatch()",
                       "static void BuildElectricalWatch()",
                       "static int ReadCbArray("):
            self.assertTrue(self._guarded(symbol),
                            "%s must stay behind #if PHOTON_DEV" % symbol)

    def test_the_ambient_blend_ships(self):
        """It is part of the LOOK, not an instrument. Left behind the guard, a
        release would draw every layer's night colour at noon — and it would be
        perfect on the dev machine."""
        for symbol in ("static void FxSampleAmbient()",
                       "static float FxAmbientNow()",
                       "static FxLayerNow FxLayerAtAmbient("):
            self.assertFalse(self._guarded(symbol),
                             "%s must not be behind #if PHOTON_DEV" % symbol)

    def test_the_colour_ramp_ships(self):
        """It is part of the LOOK. Behind the guard, every ramped layer would
        draw its full-brightness colour at every knob position in a release —
        and would be perfect on the dev machine."""
        self.assertFalse(self._guarded("static void FxLayerColorAtDrive("))

    def test_their_editors_stay_dev_only(self):
        for symbol in ("static void BuildFxAmbientPane()",
                       "static void BuildFxDrivePane()",
                       "static void FxHoldDriveOverride(",
                       "static void BuildFxLayerExtrasRow("):
            self.assertTrue(self._guarded(symbol),
                            "%s must stay behind #if PHOTON_DEV" % symbol)

    def test_the_electrical_gates_ship(self):
        """⚠ These decide whether a tint is painted over a DEAD screen, so a
        release without them is the whole point of the feature missing — and it
        would look perfect on the dev machine."""
        for symbol in ("static void BuildRectDrivers()",
                       "static FxDriver gRectBus[64]",
                       "static FxDriver gRectCb[64]",
                       "static bool DcBusLive()",
                       "static void BindDcduCommands()",
                       "static int DcduBrightnessHandler("):
            self.assertFalse(self._guarded(symbol),
                             "%s must not be behind #if PHOTON_DEV" % symbol)

    def test_driver_handles_are_dropped_on_every_aircraft_change(self):
        """⚠ THIS REGRESSED ONCE. FxForgetDrivers was called only from a
        `#if PHOTON_DEV` block while the compositor itself had already moved into
        the shipping build, so a release held XPLMDataRefs into ToLiss's plugin
        across an aircraft swap. Every symptom of that is a crash."""
        start = PLUGIN_CPP.index("static void UpdateMenuVisibility() {")
        end = PLUGIN_CPP.index("\n}\n", start)
        body = PLUGIN_CPP[start:end]
        self.assertIn("FxForgetDrivers();", body)
        self.assertIn("BindDcduCommands();", body)
        # ...and outside the guard within that function.
        guard = body.index("#if PHOTON_DEV") if "#if PHOTON_DEV" in body else len(body)
        self.assertLess(body.index("FxForgetDrivers();"), guard,
                        "FxForgetDrivers() is inside UpdateMenuVisibility's "
                        "#if PHOTON_DEV block again")
        self.assertLess(body.index("BindDcduCommands();"), guard)

    def test_only_a_dev_build_writes_the_layer_definition(self):
        """⚠ On a dev machine panelfx.txt is a SYMLINK into the repo and in a
        release it is an installed artifact an upgrade replaces. Either way a
        user-facing control writing it would be wrong, so the writer is dev-only
        and the Displays settings go somewhere else entirely."""
        self.assertIn("static void SaveDisplays()", PLUGIN_CPP)
        # SaveDisplays must not reach SavePanelFx.
        start = PLUGIN_CPP.index("static void SaveDisplays()")
        end = PLUGIN_CPP.index("\n}", start)
        self.assertNotIn("SavePanelFx", PLUGIN_CPP[start:end])


class CompositorHotPathTests(unittest.TestCase):
    """What the per-pass cost of the compositor is allowed to contain.

    Every one of these fails silently or invisibly: a GL query back inside the
    quad emitter is a GL_INVALID_OPERATION that draws nothing, a stat() back in
    the panel pass is a hitch nobody attributes to this plugin, and a value cache
    left armed freezes the Dev panes' live readouts at whatever the last frame
    saw. See docs/fcu_tint_plan.md 8j.
    """

    def _body(self, name):
        """A function's body, whatever its parameters or return type.

        Brace-matched rather than anchored on a column-0 `}` so this works on
        functions that take arguments (PaintPanelTarget, FxDriverRaw) and ones
        returning a template type by reference (FxDrawOrder) — the shapes the
        older helper in this file cannot see.
        """
        for m in re.finditer(r"\b%s\s*\([^;{]*\)\s*\{" % re.escape(name),
                             PLUGIN_CPP):
            open_at, depth = m.end() - 1, 0
            for i in range(open_at, len(PLUGIN_CPP)):
                if PLUGIN_CPP[i] == "{":
                    depth += 1
                elif PLUGIN_CPP[i] == "}":
                    depth -= 1
                    if depth == 0:
                        return PLUGIN_CPP[open_at + 1:i]
        self.fail("%s definition not found" % name)

    def _emit_rect_quad(self):
        m = re.search(r"^static void EmitRectQuad\(.*?^\}", PLUGIN_CPP, re.S | re.M)
        self.assertIsNotNone(m, "EmitRectQuad not found")
        return m.group(0)

    def test_the_quad_emitter_holds_no_glbegin_and_no_state_query(self):
        # glGetIntegerv between glBegin and glEnd is GL_INVALID_OPERATION, so the
        # winding query and the batching are ONE change: doing half of it draws
        # nothing at all, which reads as the compositor not running.
        body = self._emit_rect_quad()
        for call in ("glBegin", "glEnd", "glGetIntegerv", "glGetFloatv"):
            self.assertNotIn(call, body,
                             "%s is back inside EmitRectQuad — the quads of one "
                             "layer are one PanelQuadBatch and the batch owns "
                             "this" % call)

    def test_the_quad_emitter_refuses_to_draw_outside_a_batch(self):
        # The only way the split can be got wrong is a caller that forgets to
        # open one, and a vertex outside a begin/end block is silently dropped by
        # GL. Say so rather than leaving a blank cockpit and a clean log.
        self.assertIn("gQuadBatchOpen", self._emit_rect_quad())

    def test_the_batch_queries_the_winding_it_emits(self):
        # X-Plane leaves GL_CULL_FACE on in the panel pass, so a quad wound the
        # wrong way is culled and looks exactly like "drawing does not work
        # here". Whoever owns glBegin owns this query.
        m = re.search(r"struct PanelQuadBatch \{(.*?)^\};", PLUGIN_CPP, re.S | re.M)
        self.assertIsNotNone(m, "PanelQuadBatch not found")
        body = m.group(1)
        self.assertIn("GL_FRONT_FACE", body)
        self.assertIn("glBegin(GL_QUADS)", body)
        self.assertIn("glEnd()", body)

    def test_the_probes_clear_method_is_never_wrapped_in_a_batch(self):
        # ClearProbeRect is glScissor + glClear, neither legal inside a begin/end
        # block. It is the probe's strongest diagnostic precisely because it
        # touches no vertices; wrapping it would turn a clean negative into a GL
        # error that also draws nothing.
        m = re.search(r"PanelQuadBatch batch;\s*\n\s*PaintPanelTarget\("
                      r"gPanelProbeRect,\s*\n?\s*(\w+)", PLUGIN_CPP)
        self.assertIsNotNone(m, "the probe no longer opens a batch")
        self.assertEqual(m.group(1), "DrawProbeRect",
                         "the batched probe path must be the quad one")

    def test_group_members_resolve_to_indices_once(self):
        # RectIndexByName is a strcmp walk of the whole rect table and this ran
        # per member per layer per pass. Names stay the wire identity; only the
        # lookup is hoisted.
        body = self._body("PaintPanelTarget")
        self.assertNotIn("RectIndexByName", body,
                         "PaintPanelTarget resolves member names again — use "
                         "GroupMembers()")
        self.assertIn("GroupMembers(", body)

    def test_the_driver_value_cache_is_keyed_on_the_resolved_dataref(self):
        # Six rects on the same bus are six distinct FxDriver objects naming one
        # dataref. Keying on the object collapses the per-layer repetition and
        # leaves the per-rect repetition, which is most of it.
        body = self._body("FxDriverRaw")
        self.assertRegex(body, r"e\.ref\s*==\s*d\.ref")
        self.assertRegex(body, r"e\.index\s*==\s*d\.index")

    def test_the_driver_value_cache_is_armed_only_inside_a_pass(self):
        # The Dev panes call FxRectFactor and FxDriverState to SHOW a live value
        # — that moving number is the only thing distinguishing a driver that
        # binds and reads the wrong quantity from one that never bound. A cache
        # they read through would freeze it.
        body = self._body("DrawPanelFx")
        self.assertIn("FxValueCacheBegin();", body)
        self.assertIn("FxValueCacheEnd();", body)
        self.assertLess(body.index("FxValueCacheBegin();"),
                        body.index("FxValueCacheEnd();"))
        # And nothing else may arm it.
        self.assertEqual(PLUGIN_CPP.count("FxValueCacheBegin();"), 1)

    def test_a_failed_driver_read_is_cached_too(self):
        # An index past the end of its array takes the length probe — the most
        # expensive path in here — and can never succeed. Without caching the
        # failure it retries once per rect per layer, all pass.
        body = self._body("FxDriverRaw")
        self.assertRegex(body, r"e\.ok\s*=\s*ok", "the ok flag is not stored")
        self.assertRegex(body, r"return e\.ok;", "a cached failure re-reads")

    def test_the_compositor_touches_no_files(self):
        # The overlay poll used to stat() every image once a second from here, on
        # the render thread, in everybody's release build — for a convenience only
        # a developer editing a look ever used.
        body = self._body("DrawPanelFx")
        for call in ("FxStampOf", "ScanFxTextures", "PollFxTextureChanges",
                     "fs::exists", "fs::file_size"):
            self.assertNotIn(call, body,
                             "%s is back in the panel pass" % call)

    def test_there_is_no_automatic_overlay_poll_left(self):
        # Its whole body was a subset of ScanFxTextures, which the Rescan button
        # already calls. Two code paths deciding what "changed" means is the way
        # this grows back.
        self.assertNotRegex(
            PLUGIN_CPP, r"static void PollFxTextureChanges\s*\(",
            "PollFxTextureChanges is back — refresh is the editor's button")

    def test_the_overlay_folder_is_scanned_off_the_render_thread(self):
        # It walks a directory, may create it and may write a README. LoadPanelFx
        # runs from XPluginEnable; the lazy scan in FxTextureByName stays only as
        # a backstop.
        self.assertIn("ScanFxTextures();", self._body("LoadPanelFx"))

    def test_the_draw_order_buffer_is_reused_but_still_sorted(self):
        # Caching the ORDER would need invalidating everywhere the editor adds,
        # removes or re-targets a stack, and a stale order is a silent visual bug
        # (a base compositing over its own refinement). The allocation was the
        # cost; sorting ten ints is not.
        body = self._body("FxDrawOrder")
        self.assertIn("static std::vector<int> order;", body)
        self.assertIn("std::stable_sort", body)
        self.assertRegex(PLUGIN_CPP,
                         r"static const std::vector<int>& FxDrawOrder\(\)")


class LayerDefinitionTests(unittest.TestCase):
    """src/native/panelfx.txt — the authored look, shipped into the plugin folder."""

    def test_the_layer_definition_is_in_the_repo(self):
        self.assertTrue(PANELFX.is_file(),
                        "src/native/panelfx.txt is missing; it is the authored "
                        "look and cannot be regenerated")

    def test_every_image_a_layer_names_exists(self):
        """A layer whose image is missing is SKIPPED at runtime, not drawn as a
        solid — so the failure is a cockpit quietly missing one layer, with the
        only trace a line in Log.txt."""
        want = set()
        for line in PANELFX.read_text(encoding="utf-8").splitlines():
            parts = line.split()
            if len(parts) >= 9 and parts[0] == "layer" and parts[8] != "-":
                want.add(" ".join(parts[8:]))
        self.assertTrue(want, "no image-bearing layers found — parser drift?")
        for name in sorted(want):
            self.assertTrue((OVERLAYS / name).is_file(),
                            "%s is named by a layer but not in src/native/overlays/" % name)

    def test_the_plugin_reads_it_from_its_own_folder(self):
        # Not Output/preferences: it is a shipped artifact replaced by an
        # upgrade, on the same footing as the overlay images beside it.
        start = PLUGIN_CPP.index("static fs::path PanelFxFilePath()")
        end = PLUGIN_CPP.index("\n}", start)
        body = PLUGIN_CPP[start:end]
        self.assertIn("PhotonPluginDir()", body)
        self.assertNotIn("preferences", body)


class DeployTests(unittest.TestCase):
    """deploy.ps1 is the ONE command that gets a change into the sim (2026-08-03).

    Before that it built only the .xpl, so every session invented its own recipe
    for the other half and half of them forgot it — which is indistinguishable
    in-sim from a lighting change that did not work."""

    def _ps1(self):
        return (REPO / "src" / "native" / "deploy.ps1").read_text(
            encoding="utf-8", errors="replace")

    def test_deploy_builds_the_objs_into_the_live_install(self):
        # Without --write the OBJs land in dist/ and nothing reaches X-Plane, so
        # the deploy would report success having updated only the plugin.
        ps1 = self._ps1()
        self.assertRegex(ps1, r"build_objs\.py['\"],\s*['\"]build['\"]")
        self.assertIn("'--write'", ps1)
        self.assertIn("make_overlays.py", ps1)

    def test_deploy_defaults_to_the_all_target(self):
        # `both` is exterior + interior only. A default of `both` here would
        # leave lights_screens.obj stale on every deploy — the documented way an
        # edited screens fixture appears not to rebuild (CLAUDE.md).
        self.assertRegex(self._ps1(),
                         r"\$Target\s*=\s*[\"']all[\"']")

    def test_deploy_exports_the_xplane_root(self):
        # build_objs.py locates X-Plane on its own ($XPLANE_ROOT -> the repo's
        # Log.txt symlink -> the Steam default). Without this export, a deploy
        # with -XPlaneRoot puts the .xpl in one install and the OBJs in another
        # and neither half looks wrong on its own.
        self.assertRegex(self._ps1(),
                         r"\$env:XPLANE_ROOT\s*=\s*\$XPlaneRoot")

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


class EnableDisableSymmetryTests(unittest.TestCase):
    """Everything registered from XPluginEnable must be released by XPluginDisable.

    Plugin Admin's tick box calls the pair as many times as the user clicks it,
    and X-Plane never resets our globals in between. An unbalanced registration
    is therefore not a leak that shows up at exit — it is a duplicate that shows
    up on the SECOND enable, in this session, and stays.

    This bit once already: RegisterDbgLightRefs stands the editor down when it
    finds ToLissPhoton/debug/light/0 already registered, which after one
    disable/enable cycle it always did, because the previous cycle's own
    accessors were still there. The whole ToLissPhoton/debug/* family went inert
    for the rest of the session and the log blamed PI_PhotonDevReload.py."""

    def _body(self, name):
        m = re.search(r"^[\w ]*\b%s\(\s*(?:void)?\s*\)\s*\{(.*?)^\}"
                      % re.escape(name), PLUGIN_CPP, re.S | re.M)
        self.assertIsNotNone(m, "%s not found" % name)
        return m.group(1)

    def test_the_debug_light_pool_is_released_when_the_plugin_is_disabled(self):
        # Registered from StartPanelPass, so it must be dropped from StopPanelPass
        # — the two are XPluginEnable's and XPluginDisable's halves of the pool.
        self.assertIn("RegisterDbgLightRefs();", self._body("StartPanelPass"))
        self.assertIn("UnregisterDbgLightRefs();", self._body("StopPanelPass"))

    def test_unregistering_the_pool_clears_the_ownership_flag(self):
        # Leaving gDbgOwnsRefs true would keep the Lights tab presenting knobs
        # for accessors that no longer exist.
        body = self._body("UnregisterDbgLightRefs")
        self.assertRegex(body, r"gDbgOwnsRefs\s*=\s*false;")

    def test_every_debug_accessor_that_is_registered_is_also_unregistered(self):
        reg = self._body("RegisterDbgLightRefs")
        unreg = self._body("UnregisterDbgLightRefs")
        handles = set(re.findall(r"(gDbg\w*Refs?)\s*(?:\[[^\]]*\])?\s*=\s*"
                                 r"XPLMRegisterDataAccessor", reg))
        self.assertTrue(handles, "no registrations found — did the pool move?")
        for h in sorted(handles):
            self.assertIn(h, unreg,
                          "%s is registered but never unregistered" % h)

    def test_the_dcdu_command_handlers_do_not_outlive_a_disable(self):
        # They are registered from UpdateMenuVisibility, which XPluginEnable
        # calls. A disabled plugin still counting button presses would publish a
        # ToLissPhoton/screens/dcdu_brightness the real screen never reached.
        self.assertIn("UnbindDcduCommands();", self._body("XPluginDisable"))


if __name__ == "__main__":
    unittest.main()

"""The dev plugin's Live Light Debug accessors, exercised against a stub `xp`.

The first version of this feature registered nothing and drove nothing, and both
failures were invisible from the repo — a wrong keyword argument and two XPPython3 list
conventions, all of which show up only as "the lights just sit there" in a running sim.
XPPython3 cannot be imported outside X-Plane, so the module loads against a stub that
reproduces the contracts that matter:

  * `registerDataAccessor` accepts `readRefCon`/`writeRefCon` and REJECTS `refCon`
  * an array read callback gets an EMPTY list to `.extend()`, never a sized buffer
  * `count == -1` means "all elements", not "none"
"""
from __future__ import annotations

import importlib.util
import json
import math
import os
import sys
import tempfile
import types
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PLUGIN = REPO / "src" / "plugin" / "PI_PhotonDevReload.py"


class StubDataRef:
    def __init__(self, name, values):
        self.name = name
        self.values = values


def _make_xp():
    """A stub of the XPPython3 `xp` module, faithful where it matters."""
    xp = types.ModuleType("xp")

    xp.Type_Int, xp.Type_Float, xp.Type_Double = 1, 2, 4
    xp.Type_FloatArray, xp.Type_IntArray = 8, 16
    xp.NO_PLUGIN_ID = -1
    xp.accessors = {}          # name -> kwargs, for the test to inspect
    xp.messages = []
    xp.datarefs = {}           # name -> StubDataRef
    xp.elapsed = 0.0
    xp.logged = []

    def registerDataAccessor(name, dataType=0, writable=-1, **kw):
        # The real signature has readRefCon/writeRefCon and NO plain `refCon`;
        # passing one is a TypeError, which is precisely the bug this pins.
        allowed = {"readInt", "writeInt", "readFloat", "writeFloat", "readDouble",
                   "writeDouble", "readIntArray", "writeIntArray", "readFloatArray",
                   "writeFloatArray", "readData", "writeData", "readRefCon",
                   "writeRefCon"}
        bad = set(kw) - allowed
        if bad:
            raise TypeError("%r is an invalid keyword argument for this function"
                            % sorted(bad)[0])
        xp.accessors[name] = kw
        return StubDataRef(name, None)

    def unregisterDataAccessor(ref):
        xp.accessors.pop(getattr(ref, "name", None), None)

    def findDataRef(name):
        return xp.datarefs.get(name)

    def getDatavf(ref, values=None, offset=0, count=-1):
        src = ref.values
        if values is None:
            return len(src)
        if count < 0:
            count = len(src)
        out = src[offset:offset + count]
        values.extend(out)          # EXTEND, matching the real API
        return len(out)

    def getDataRefTypes(ref):
        return xp.Type_Int if isinstance(ref.values, int) else xp.Type_FloatArray

    xp.registerDataAccessor = registerDataAccessor
    xp.unregisterDataAccessor = unregisterDataAccessor
    xp.findDataRef = findDataRef
    xp.getDatavf = getDatavf
    xp.getDataRefTypes = getDataRefTypes
    xp.getDatai = lambda ref: int(ref.values)
    xp.getDataf = lambda ref: float(ref.values)
    xp.getDatad = lambda ref: float(ref.values)
    xp.getElapsedTime = lambda: xp.elapsed
    xp.log = xp.logged.append
    xp.createCommand = lambda name, desc: StubDataRef(name, None)
    xp.registerCommandHandler = lambda *a, **k: None
    xp.unregisterCommandHandler = lambda *a, **k: None
    xp.findPluginBySignature = lambda sig: xp.NO_PLUGIN_ID
    xp.sendMessageToPlugin = lambda pid, msg, payload: xp.messages.append(
        (pid, msg, payload))
    return xp


def load_plugin_module():
    xp = _make_xp()
    pkg = types.ModuleType("XPPython3")
    pkg.xp = xp
    sys.modules["XPPython3"] = pkg
    sys.modules["XPPython3.xp"] = xp
    spec = importlib.util.spec_from_file_location("PI_PhotonDevReload_test", PLUGIN)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod, xp


def MARKER_DATAREFS(mod):
    """The four datarefs the position markers add, in one place — three tests assert
    the full registration set and would otherwise each need updating separately."""
    return ([mod.DebugMarkerDataRef, mod.DebugMarkerShowDataRef]
            + ["%s/%d" % (mod.DebugMarkerParamDataRef, k)
               for k in range(mod.DebugMarkerRoles)])


class AccessorRegistrationTests(unittest.TestCase):
    def test_the_whole_pool_registers(self):
        """The bug that shipped: `refCon=` instead of `readRefCon=` raised TypeError on
        every call, so ZERO slots registered. The loop breaks on first failure, so a
        count of 0 is the exact signature of that mistake."""
        mod, xp = load_plugin_module()
        p = mod.PythonInterface()
        p.XPluginStart()
        self.assertEqual(len(p.dbgAccessors), mod.DebugMaxLights)
        self.assertEqual(sorted(xp.accessors),
                         sorted([mod.DebugDataRefPrefix + str(n)
                                 for n in range(mod.DebugMaxLights)]
                                + [mod.DebugPosDataRef, mod.DebugCompareDataRef]
                                + MARKER_DATAREFS(mod)))

    def test_it_stands_down_when_the_native_plugin_already_owns_the_pool(self):
        """A PHOTON_DEV .xpl carries the same editor and registers the same names.

        Two owners cannot share them: an accessor here is a read-only COMPUTED
        value, so the other side cannot write through it — whichever registers
        second drives nothing while appearing to work, and a light that ignores
        the window looks exactly like a bad OBJ. Whoever is first wins; the
        native side makes the same check."""
        mod, xp = load_plugin_module()
        xp.datarefs[mod.DebugDataRefPrefix + "0"] = StubDataRef(
            mod.DebugDataRefPrefix + "0", [0.0] * mod.DebugParamCount)
        p = mod.PythonInterface()
        p.XPluginStart()
        self.assertFalse(p.dbgOwnsRefs)
        self.assertEqual(p.dbgAccessors, [])
        # Not one slot, not the position array, not the markers: registering any
        # of them would be a second owner of that particular name.
        for name in ([mod.DebugPosDataRef, mod.DebugCompareDataRef]
                     + MARKER_DATAREFS(mod)):
            self.assertNotIn(name, xp.accessors)

    def test_it_owns_the_pool_when_nothing_else_has_it(self):
        # The other half of the branch — without this the test above passes just
        # as well against a script that never registers anything.
        mod, xp = load_plugin_module()
        p = mod.PythonInterface()
        p.XPluginStart()
        self.assertTrue(p.dbgOwnsRefs)

    def test_the_ab_toggle_registers_int_and_float_readers(self):
        """An OBJ's ANIM_hide reads its dataref as a FLOAT. With only readInt registered
        X-Plane advertises the type wrong, the gate reads 0, and the A/B toggle sticks
        on "original"."""
        mod, xp = load_plugin_module()
        p = mod.PythonInterface()
        p.XPluginStart()
        kw = xp.accessors[mod.DebugCompareDataRef]
        self.assertIn("readInt", kw)
        self.assertIn("readFloat", kw)
        self.assertEqual(kw["readInt"](None), 1)      # defaults to the debug copies
        self.assertEqual(kw["readFloat"](None), 1.0)
        p._dbg_toggle_compare()
        self.assertEqual(kw["readInt"](None), 0)
        self.assertEqual(kw["readFloat"](None), 0.0)

    def test_each_slot_carries_its_own_index_as_the_read_refcon(self):
        """One callback serves all 64 slots; the refCon is the only thing telling them
        apart. If it were shared or absent every light would read light 0's params."""
        mod, xp = load_plugin_module()
        p = mod.PythonInterface()
        p.XPluginStart()
        for n in range(mod.DebugMaxLights):
            kw = xp.accessors[mod.DebugDataRefPrefix + str(n)]
            self.assertEqual(kw["readRefCon"], n)
            # == not `is`: attribute access makes a fresh bound-method object each
            # time, but two of them compare equal when func and instance match.
            self.assertEqual(kw["readFloatArray"], p.DbgReadArray)

    def test_datarefs_are_announced_to_browsers(self):
        """Custom datarefs are not discoverable — a browser lists only what it is told
        about, so without this the accessors work and appear nowhere."""
        mod, xp = load_plugin_module()
        xp.findPluginBySignature = lambda sig: 7 if "datareftool" in sig else xp.NO_PLUGIN_ID
        p = mod.PythonInterface()
        p.XPluginStart()
        names = [payload for _, msg, payload in xp.messages if msg == 0x01000000]
        # the light pool + position + compare + the four marker datarefs
        self.assertEqual(len(names), mod.DebugMaxLights + 2 + len(MARKER_DATAREFS(mod)))
        self.assertIn(mod.DebugDataRefPrefix + "0", names)
        self.assertIn(mod.DebugPosDataRef, names)
        self.assertIn(mod.DebugCompareDataRef, names)
        for name in MARKER_DATAREFS(mod):
            self.assertIn(name, names)

    def test_unregister_clears_the_pool(self):
        mod, xp = load_plugin_module()
        p = mod.PythonInterface()
        p.XPluginStart()
        p.XPluginStop()
        self.assertEqual(xp.accessors, {})
        self.assertEqual(p.dbgAccessors, [])


def _loaded(mod, xp, lights=None, pos_tunable=True):
    """A PythonInterface with a two-light manifest already loaded.

    `pos_tunable` mirrors the manifest key of the same name: True stands in for an OBJ
    built with --debug-pos (ANIM_trans wrappers present), False for a plain --debug one."""
    p = mod.PythonInterface()
    p.XPluginStart()
    p.dbgPosTunable = pos_tunable
    p.dbgLights = lights if lights is not None else [
        {"n": 0, "name": "a", "fixture": "fx", "category": "dome",
         "source": "panel", "index": 0, "mount": None, "pos": [0, 0, 0],
         "rgb": [1.0, 0.5, 0.25], "size": 2.0, "dir": [0, -1, 0], "cone": 0.9},
        {"n": 1, "name": "b", "fixture": "fx", "category": "console",
         "source": "inst", "index": 22, "mount": None, "pos": [1, 0, 0],
         "rgb": [0.2, 0.4, 0.6], "size": 0.4, "dir": [0, -1, 0], "cone": 0.4},
    ]
    p.dbgLoadTried = True          # suppress the lazy load; this manifest is injected
    p.dbgVals, p.dbgBase = [], []
    for light in p.dbgLights:
        # Mirrors _dbg_load's own seeding, including the aim tail: pitch/yaw derived
        # from the authored vector, then the vector re-derived from them. Skipping the
        # re-derivation here would let a test pass on a non-unit vector the real loader
        # could never produce.
        aim = mod._dir_to_aim(light["dir"])
        v = [light["rgb"][0], light["rgb"][1], light["rgb"][2], 1.0, light["size"],
             float(light["dir"][0]), float(light["dir"][1]), float(light["dir"][2]),
             light["cone"], 0.0, 0.0, 0.0, round(aim[0], 3), round(aim[1], 3)]
        p.dbgVals.append(list(v))
        p._dbg_apply_aim(len(p.dbgVals) - 1)
        p.dbgBase.append(list(p.dbgVals[-1]))
    xp.datarefs[mod.DebugPanelRheostat] = StubDataRef(mod.DebugPanelRheostat,
                                                      [1.0, 0.0, 0.0, 0.0])
    xp.datarefs[mod.DebugInstRheostat] = StubDataRef(mod.DebugInstRheostat,
                                                     [0.0] * 24 + [1.0] * 8)
    xp.datarefs[mod.DebugMapRheostat] = StubDataRef(mod.DebugMapRheostat, 1)
    return p


class ReadCallbackTests(unittest.TestCase):
    def test_size_query_returns_the_param_count(self):
        """`values is None` is X-Plane asking how big the array is. Returning 0 (or
        raising) makes the OBJ bind a zero-length param list."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        self.assertEqual(p.DbgReadArray(0, None, 0, -1), mod.DebugParamCount)

    def test_full_read_extends_the_list(self):
        """XPPython3 hands an EMPTY list to extend, not a sized buffer. Indexing raises
        IndexError, the except swallows it into a return of 0, and the light draws
        nothing — silently, forever."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        out = []
        n = p.DbgReadArray(0, out, 0, mod.DebugParamCount)
        self.assertEqual(n, mod.DebugParamCount)
        self.assertEqual(len(out), mod.DebugParamCount)
        # WIRE order: r g b a size SEMI dx dy dz. The cone is at index 5, not 8 — see
        # WireOrderTests. This test asserted the readable order until 2026-07-30.
        self.assertEqual(out[:3], [1.0, 0.5, 0.25])       # rgb
        self.assertEqual(out[4], 2.0)                      # size
        self.assertEqual(out[5], 0.9)                      # semi (the cone)
        self.assertEqual([round(x, 6) for x in out[6:9]], [0.0, -1.0, 0.0])   # dir

    def test_count_minus_one_means_all_of_them(self):
        """-1 is "give me everything". A `range(count)` loop treats it as "none"."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        out = []
        self.assertEqual(p.DbgReadArray(0, out, 0, -1), mod.DebugParamCount)
        self.assertEqual(len(out), mod.DebugParamCount)

    def test_partial_read_honours_offset(self):
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        out = []
        self.assertEqual(p.DbgReadArray(0, out, 4, 2), 2)
        self.assertEqual(out, [2.0, 0.9])                  # size, then SEMI (wire order)

    def test_a_slot_past_the_manifest_is_a_dark_light(self):
        """The pool is bigger than any manifest, so unused slots must read as a light
        that draws nothing rather than as an error."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        out = []
        p.DbgReadArray(40, out, 0, -1)
        self.assertEqual(out, [0.0] * mod.DebugParamCount)


class BrightnessAndIdentifyTests(unittest.TestCase):
    def test_alpha_follows_the_light_s_own_rheostat(self):
        """LIGHT_SPILL_CUSTOM has no INDEX slot, so the plugin multiplies the rheostat
        in itself. Light 0 is on panel[0] (stub: 1.0), light 1 on inst[22] (stub: 0.0),
        so they must differ."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        self.assertEqual(p._dbg_params(0)[3], 1.0)
        self.assertEqual(p._dbg_params(1)[3], 0.0)
        xp.datarefs[mod.DebugInstRheostat].values = [0.0] * 22 + [0.5] * 10
        xp.elapsed += 1.0                        # the per-frame cache must expire
        self.assertEqual(p._dbg_params(1)[3], 0.5)

    def test_isolate_darkens_everything_but_the_selection(self):
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        xp.datarefs[mod.DebugInstRheostat].values = [1.0] * 32
        p.dbgIsolate, p.dbgSel = True, 0
        self.assertEqual(p._dbg_params(0)[3], 1.0)
        self.assertEqual(p._dbg_params(1)[3], 0.0)

    def test_mark_paints_only_the_selected_light(self):
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        p.dbgMark, p.dbgSel = True, 1
        self.assertEqual(tuple(p._dbg_params(1)[:3]), mod.DebugMarkColor)
        self.assertEqual(tuple(p._dbg_params(0)[:3]), (1.0, 0.5, 0.25))

    def test_blink_is_a_square_wave_on_the_selection_only(self):
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        p.dbgBlink, p.dbgSel = True, 0
        xp.elapsed = 0.0
        self.assertEqual(p._dbg_params(0)[3], 1.0)          # first half of the cycle
        xp.elapsed = 0.5 / mod.DebugBlinkHz + 1e-6
        self.assertEqual(p._dbg_params(0)[3], 0.0)          # second half

    def test_a_missing_rheostat_does_not_black_the_light_out(self):
        """If the dataref name is ever wrong, defaulting to 0 would look exactly like
        the tool being broken. Default to full brightness and say so in the log."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        xp.datarefs.pop(mod.DebugPanelRheostat)
        p.dbgRheostatRefs.clear()
        xp.elapsed += 1.0
        self.assertEqual(p._dbg_params(0)[3], 1.0)


class PositionOffsetTests(unittest.TestCase):
    """Position is live via three dataref-driven ANIM_trans per light, reading a
    single shared array as an OFFSET in metres from the baked spot."""

    def test_the_position_array_registers_and_sizes_itself(self):
        mod, xp = load_plugin_module()
        p = mod.PythonInterface()
        p.XPluginStart()
        self.assertIn(mod.DebugPosDataRef, xp.accessors)
        self.assertEqual(p.DbgReadPos(0, None, 0, -1),
                         mod.DebugMaxLights * mod.DebugPosCount)

    def test_each_light_reads_its_own_three_elements(self):
        """`ANIM_trans` references [3n+axis], so the mapping from array index to
        (light, axis) has to be exactly this. Off by one and every light is nudged
        by its neighbour's offset."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        p.dbgVals[0][mod.DebugPosSlot:] = [0.1, 0.2, 0.3]
        p.dbgVals[1][mod.DebugPosSlot:] = [-0.4, 0.0, 0.5]
        out = []
        p.DbgReadPos(0, out, 0, 6)
        self.assertEqual([round(x, 3) for x in out], [0.1, 0.2, 0.3, -0.4, 0.0, 0.5])

    def test_single_element_read_is_what_anim_trans_actually_does(self):
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        p.dbgVals[1][mod.DebugPosSlot + 2] = 0.75
        out = []
        self.assertEqual(p.DbgReadPos(0, out, 5, 1), 1)     # light 1, axis z
        self.assertEqual(out, [0.75])

    def test_offsets_default_to_zero_so_a_fresh_load_does_not_move_anything(self):
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        out = []
        p.DbgReadPos(0, out, 0, -1)
        self.assertEqual(set(out), {0.0})

    def test_nudge_clamps_to_the_anim_keyframe_span(self):
        """X-Plane clamps at the outermost keyframe, so a value beyond the span moves
        nothing while the window would still show it climbing."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        p.dbgSel, p.dbgStep = 0, 1.0
        for _ in range(200):
            p._dbg_nudge(mod.DebugPosSlot, "pos", +1)
        self.assertEqual(p.dbgVals[0][mod.DebugPosSlot], mod.DebugPosRange)
        for _ in range(400):
            p._dbg_nudge(mod.DebugPosSlot, "pos", -1)
        self.assertEqual(p.dbgVals[0][mod.DebugPosSlot], -mod.DebugPosRange)

    def test_absolute_position_is_baked_plus_offset(self):
        """What "Log DSL" prints, and what goes into the .phdsl — the window works in
        offsets but nothing downstream does."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        p.dbgVals[1][mod.DebugPosSlot:] = [0.25, -0.5, 1.0]
        self.assertEqual([round(x, 3) for x in p._dbg_abs_pos(1)], [1.25, -0.5, 1.0])

    def test_plugin_and_generator_agree_on_the_keyframe_span(self):
        """DebugPosRange is the ANIM_trans keyframe span baked into the OBJ. If the two
        drift, every position edit is silently rescaled — a 10 cm nudge moves 5 cm."""
        mod, _ = load_plugin_module()
        sys.path.insert(0, str(REPO / "build"))
        import build_objs as B
        self.assertEqual(mod.DebugPosRange, B.DEBUG_POS_RANGE)
        self.assertEqual(mod.DebugPosDataRef, B.DEBUG_POS_DREF)


class AimAngleTests(unittest.TestCase):
    """Aim is edited as pitch/yaw in degrees; dx/dy/dz are derived from it.

    Raw vector editing was reported in-sim as "very difficult to get things pointed
    correctly", and it has a second failure mode beyond awkwardness: a hand-edited
    vector's LENGTH is not free, because X-Plane compares `cone` against a dot product
    with it. Angles cannot express a bad length at all."""

    def test_the_convention_is_the_one_the_docs_state(self):
        """0 pitch / 0 yaw is FORWARD, yaw runs clockwise from above, pitch is degrees
        above horizontal. Getting a sign backwards here mis-aims every light that goes
        through the window, so it is pinned against named directions."""
        mod, _ = load_plugin_module()
        cases = {
            (0, 0):     [0, 0, -1],       # forward
            (0, 90):    [1, 0, 0],        # right
            (0, 180):   [0, 0, 1],        # aft
            (0, -90):   [-1, 0, 0],       # left
            (90, 0):    [0, 1, 0],        # straight up
            (-90, 0):   [0, -1, 0],       # straight down
        }
        for (pitch, yaw), want in cases.items():
            got = mod._aim_to_dir(pitch, yaw)
            for g, w in zip(got, want):
                self.assertAlmostEqual(g, w, places=6,
                                       msg="aim %s %s -> %s" % (pitch, yaw, got))

    def test_aim_round_trips_through_the_vector(self):
        mod, _ = load_plugin_module()
        for pitch in (-89, -60, -30, 0, 30, 60, 89):
            for yaw in (-180, -90, -37, 0, 45, 90, 179):
                d = mod._aim_to_dir(pitch, yaw)
                back = mod._dir_to_aim(d)
                self.assertAlmostEqual(back[0], pitch, places=4)
                self.assertAlmostEqual((back[1] - yaw + 180) % 360 - 180, 0, places=4)

    def test_a_derived_vector_is_always_unit_length(self):
        """The other half of the point: the window can no longer emit the non-unit
        vectors that made the interior's cones inert."""
        mod, _ = load_plugin_module()
        for pitch in (-90, -45, 0, 45, 90):
            for yaw in (0, 73, 180, -120):
                d = mod._aim_to_dir(pitch, yaw)
                self.assertAlmostEqual(math.sqrt(sum(v * v for v in d)), 1.0, places=9)

    def test_editing_pitch_rewrites_the_light_s_direction(self):
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp, lights=[
            {"n": 0, "name": "a", "fixture": "fx", "category": "dome",
             "source": "panel", "index": 0, "mount": None, "pos": [0.0, 0.0, 0.0],
             "rgb": [1, 1, 1], "size": 1.0, "dir": [0, 0, -1], "cone": 0.9}])
        self.assertEqual([round(v, 3) for v in p.dbgVals[0][mod.DebugAimSlot:]],
                         [0.0, 0.0])
        p.dbgStep = 1.0
        for _ in range(30):
            p._dbg_nudge(mod.DebugAimSlot, "pitch", -1)
        self.assertAlmostEqual(p.dbgVals[0][mod.DebugAimSlot], -30.0, places=6)
        # ...and the vector followed: 30 deg down, still forward, still unit.
        d = p.dbgVals[0][5:8]
        self.assertAlmostEqual(d[1], math.sin(math.radians(-30)), places=5)
        self.assertAlmostEqual(math.sqrt(sum(v * v for v in d)), 1.0, places=6)

    def test_pitch_clamps_but_yaw_wraps(self):
        """Past +-90 pitch would come back down while yaw silently flipped 180 — an aim
        that cannot be steered. Yaw has no seam: -1 and 359 are one heading."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        p.dbgStep = 1.0
        for _ in range(200):
            p._dbg_nudge(mod.DebugAimSlot, "pitch", +1)
        self.assertEqual(p.dbgVals[0][mod.DebugAimSlot], 90.0)
        for _ in range(400):
            p._dbg_nudge(mod.DebugAimSlot, "pitch", -1)
        self.assertEqual(p.dbgVals[0][mod.DebugAimSlot], -90.0)
        p.dbgVals[0][mod.DebugAimSlot + 1] = 179.0
        p._dbg_nudge(mod.DebugAimSlot + 1, "yaw", +1)
        self.assertEqual(p.dbgVals[0][mod.DebugAimSlot + 1], -180.0)

    def test_an_omnidirectional_light_keeps_no_aim(self):
        """dir 0 0 0 is omni (the dome). Giving it an aim would silently turn a
        floodlight into a spotlight, so the angle rows leave it alone."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp, lights=[
            {"n": 0, "name": "dome", "fixture": "fx", "category": "dome",
             "source": "panel", "index": 0, "mount": None, "pos": [0.0, 0.0, 0.0],
             "rgb": [1, 1, 1], "size": 1.0, "dir": [0, 0, 0], "cone": 1.0}])
        p.dbgStep = 1.0
        p._dbg_nudge(mod.DebugAimSlot, "pitch", -1)
        self.assertEqual(p.dbgVals[0][5:8], [0.0, 0.0, 0.0])

    def test_spread_is_edited_in_degrees_and_stored_as_a_cosine(self):
        """The row shows and steps an ANGLE; slot 8 keeps the cosine the light reads.
        cone runs backwards, so widening the beam must LOWER the stored value — if that
        inverts, every spread edit goes the wrong way."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        p.dbgVals[0][8] = mod._spread_to_cone(30.0)
        p.dbgStep = 1.0
        p._dbg_nudge(8, "spread", +1)              # widen by 1 degree
        self.assertAlmostEqual(mod._cone_to_spread(p.dbgVals[0][8]), 31.0, places=4)
        self.assertLess(p.dbgVals[0][8], mod._spread_to_cone(30.0))

    def test_spread_clamps_short_of_the_degenerate_ends(self):
        """At 0 the beam is a line and at 90 the rim tangent runs to infinity, taking
        the cone marker off to the horizon with it."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        p.dbgStep = 1.0
        for _ in range(200):
            p._dbg_nudge(8, "spread", +1)
        self.assertLessEqual(mod._cone_to_spread(p.dbgVals[0][8]), 89.9 + 1e-6)
        for _ in range(200):
            p._dbg_nudge(8, "spread", -1)
        self.assertGreaterEqual(mod._cone_to_spread(p.dbgVals[0][8]), 0.1 - 1e-6)

    def test_plugin_and_dsl_agree_on_the_angle_conventions(self):
        """The window's numbers get pasted into the .phdsl as `aim:`/`spread:`, so the
        two conversions must be the same function. If they drift, a light tuned in-sim
        points somewhere else once it is authored — with nothing on screen to say so."""
        mod, _ = load_plugin_module()
        sys.path.insert(0, str(REPO / "build"))
        import photon_dsl as P
        for pitch in (-90, -55, -12, 0, 23, 90):
            for yaw in (-180, -90, 0, 37, 90, 175):
                # The DSL rounds to the project's 3 dp on the way into an OBJ; the
                # plugin keeps full precision because it feeds a dataref. Same
                # function, so they must agree once rounded the same way.
                self.assertEqual([round(v, 3) for v in mod._aim_to_dir(pitch, yaw)],
                                 [round(v, 3) for v in P.aim_to_dir(pitch, yaw)],
                                 "aim %s %s" % (pitch, yaw))
        for deg in (0.5, 14, 26, 35, 70, 89):
            self.assertAlmostEqual(mod._spread_to_cone(deg), P.spread_to_cone(deg),
                                   places=4)
        for cone in (0.1, 0.35, 0.643, 0.9, 0.97):
            self.assertAlmostEqual(mod._cone_to_spread(cone), P.cone_to_spread(cone),
                                   places=2)

    def test_the_cardinal_presets_are_the_six_axes(self):
        """Six unambiguous aims, one click each. Sweeping an angle and watching a pool
        move through a cockpit full of surfaces can be read several ways — which is how
        "pitching up makes it point right" became hard to interpret. A preset makes the
        question binary: click Up, did it go up?"""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        want = {
            "Fwd":   [0.0, 0.0, -1.0],
            "Aft":   [0.0, 0.0, 1.0],
            "Left":  [-1.0, 0.0, 0.0],
            "Right": [1.0, 0.0, 0.0],
            "Up":    [0.0, 1.0, 0.0],
            "Down":  [0.0, -1.0, 0.0],
        }
        self.assertEqual({lbl for lbl, _, _ in mod.DebugAimPresets}, set(want))
        for label, pitch, yaw in mod.DebugAimPresets:
            p._dbg_aim_preset(pitch, yaw, label)
            got = [round(v, 6) + 0.0 for v in p.dbgVals[0][5:8]]
            for g, w in zip(got, want[label]):
                self.assertAlmostEqual(g, w, places=6,
                                       msg="%s -> %s" % (label, got))
            self.assertEqual(p.dbgVals[0][mod.DebugAimSlot], pitch)
            self.assertEqual(p.dbgVals[0][mod.DebugAimSlot + 1], yaw)

    def test_a_preset_clears_a_leftover_orientation_hypothesis(self):
        """A preset has to establish a KNOWN state or it answers nothing. With a stale
        permutation still applied, "I clicked Up" would not determine what was asked,
        and that is indistinguishable in the cockpit from the sim mis-reading the axes —
        the exact confusion the preset exists to resolve."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        p.dbgOrientDir = [o[2] for o in mod.DebugOrientations].index("Y X Z")
        p._dbg_aim_preset(90.0, 0.0, "Up")
        self.assertEqual(p.dbgOrientDir, 0)
        # ...and with the hypothesis cleared, the light really does get "up".
        self.assertEqual([round(v, 6) for v in p._dbg_params(0)[5:8]],
                         [0.0, 1.0, 0.0])

    def test_the_window_rows_are_angles_not_vector_components(self):
        """The whole change. If Dir X/Y/Z ever come back, so does the problem."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        kinds = [k for _, _, k in p._dbg_slots()]
        for gone in ("dir", "cone"):
            self.assertNotIn(gone, kinds)
        for wanted in ("pitch", "yaw", "spread"):
            self.assertIn(wanted, kinds)
        labels = [lbl for lbl, _, _ in p._dbg_slots()]
        self.assertNotIn("Dir X", labels)
        self.assertIn("Pitch", labels)


class WireOrderTests(unittest.TestCase):
    """The dataref's param order is NOT the OBJ line's, measured in-sim 2026-07-30.

        OBJ8 line:  ... <size> <dx> <dy> <dz> <semi> <dataref>
        dataref:    r g b a size  SEMI dx dy dz

    The cone comes FIRST in the trailing group of four. This is what made every
    `--debug` light mis-aimed while the shipped ones were fine, and it is what the
    48-permutation axis hunt could never have found — a rotation of four slots is not an
    axis permutation."""

    def test_the_wire_order_is_the_measured_one(self):
        mod, _ = load_plugin_module()
        self.assertEqual(mod.DebugDataRefOrder, (0, 1, 2, 3, 4, 8, 5, 6, 7))
        self.assertEqual(mod.DebugWireNames,
                         ("r", "g", "b", "a", "size", "semi", "dx", "dy", "dz"))
        # a permutation, not a mangling: every slot appears exactly once
        self.assertEqual(sorted(mod.DebugDataRefOrder), list(range(9)))

    def test_the_accessor_emits_in_wire_order(self):
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        v = p.dbgVals[0]
        v[:9] = [0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9]   # dir .6 .7 .8, cone .9
        out = []
        p.DbgReadArray(0, out, 0, -1)
        self.assertEqual([round(x, 6) for x in out],
                         [0.1, 0.2, 0.3, 0.4, 0.5, 0.9, 0.6, 0.7, 0.8])

    def test_a_partial_read_slices_the_wire_array_not_the_readable_one(self):
        """X-Plane asking for [5:9] must get semi/dx/dy/dz. Reordering after the slice
        would hand it dx/dy/dz/cone — the original bug, reintroduced one layer down."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        p.dbgVals[0][:9] = [0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9]
        out = []
        self.assertEqual(p.DbgReadArray(0, out, 5, 4), 4)
        self.assertEqual([round(x, 6) for x in out], [0.9, 0.6, 0.7, 0.8])

    def test_alpha_is_index_3_in_both_orders(self):
        """Why the SHIPPING plugin needed no change: it writes only the alpha slot and
        passes the rest through, and alpha does not move. If this ever fails,
        kSpillAlphaSlot in plugin.cpp is wrong too and the map spot and screen glow go
        dark or full-bright."""
        mod, _ = load_plugin_module()
        self.assertEqual(mod.DebugDataRefOrder.index(3), 3)
        cpp = (REPO / "src" / "native" / "src" / "plugin.cpp").read_text(
            encoding="utf-8", errors="replace")
        self.assertIn("kSpillAlphaSlot  = 3", cpp)

    def test_the_obj_still_bakes_the_literal_order(self):
        """Only the DATAREF is rotated. The OBJ line keeps `dx dy dz semi`, which is
        provable from the sim's own OBJs: read that way their vectors are unit length,
        read the other way they are not. So the generator must NOT be 'fixed' to match
        the wire order."""
        sys.path.insert(0, str(REPO / "build"))
        import build_objs as B
        cfg = B.load_config(wing="stock")
        text = B.Emitter(cfg, "a320", "stock", target="screens").emit()
        line = next(l.split() for l in text.splitlines()
                    if l.strip().startswith("LIGHT_SPILL_CUSTOM"))
        # LIGHT_SPILL_CUSTOM x y z r g b a size dx dy dz semi <dref>
        d = [float(x) for x in line[9:12]]
        semi = float(line[12])
        self.assertAlmostEqual(math.sqrt(sum(v * v for v in d)), 1.0, places=3,
                               msg="dir must be unit in OBJ order dx dy dz semi")
        self.assertLess(semi, 1.0)
        self.assertGreater(semi, 0.0)


class SlotProbeTests(unittest.TestCase):
    """Sends the baseline with ONE dataref slot set, to MEASURE what each slot does.

    Everything in this project assumed the nine floats are `r g b a size dx dy dz cone`,
    read off the OBJ8 spec and `lights.txt` and never verified in-sim. It does not hold:
    2026-07-30, `aim Fwd` pointed a screen light straight down, pitch swung it
    horizontally, and the cone and a direction component affected each other. Two
    successive explanations (an axis permutation, then non-unit `dir` lengths) each fitted
    part of the evidence and were wrong, which is why this measures instead of inferring.

    A probe is only worth anything if it has exactly one variable, so these tests are
    mostly about what the probe must NOT do."""

    def test_the_probe_sends_the_baseline_with_one_slot_set(self):
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        for slot in range(mod.DebugParamCount):
            p._dbg_set_probe(slot)
            got = p._dbg_params(p.dbgSel)
            want = list(mod.DebugProbeBaseline)
            want[slot] = mod.DebugProbeValue
            self.assertEqual(got, want, "slot %d" % slot)

    def test_the_baseline_is_omnidirectional_with_a_zero_cone(self):
        """Both are load-bearing. dir 0 0 0 means a probe of 5/6/7 introduces the ONLY
        direction present, so nothing has to be disentangled from it; cone 0 means that
        if slot 8 turns out to be a direction component after all it contributes zero,
        keeping those probes clean. And `size` has to be metres-large or the pool lands
        on the panel 2 cm away, which is what made cone changes look inert."""
        mod, _ = load_plugin_module()
        base = mod.DebugProbeBaseline
        self.assertEqual(len(base), mod.DebugParamCount)
        self.assertEqual(list(base[5:8]), [0.0, 0.0, 0.0])
        self.assertEqual(base[8], 0.0)
        self.assertEqual(list(base[:4]), [1.0, 1.0, 1.0, 1.0])
        self.assertGreaterEqual(base[4], 1.0)

    def test_probing_bypasses_every_other_modifier(self):
        """Orientation, rheostat, Isolate, Blink and Mark would each be a second variable
        in an experiment designed to have one."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        p.dbgOrientDir = [o[2] for o in mod.DebugOrientations].index("Y X Z")
        p.dbgMark = p.dbgBlink = p.dbgIsolate = True
        p.dbgVals[0][3] = 0.0                    # alpha the rheostat path would scale
        p._dbg_set_probe(6)
        got = p._dbg_params(0)
        self.assertEqual(got[5:8], [0.0, 1.0, 0.0], "orientation must not be applied")
        self.assertEqual(got[3], 1.0, "alpha must be exactly the baseline's")
        self.assertEqual(got[:3], [1.0, 1.0, 1.0], "Mark must not recolour a probe")

    def test_every_other_light_goes_dark_while_probing(self):
        """Two lit lights and you cannot say which one moved."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        p._dbg_set_probe(5)
        self.assertEqual(p._dbg_params(1), [0.0] * mod.DebugParamCount)
        p.dbgSel = 1
        self.assertEqual(p._dbg_params(0), [0.0] * mod.DebugParamCount)

    def test_the_marker_draws_the_probe_s_own_direction(self):
        """The arrow is the reference and the light is the sim's answer; side by side,
        that IS the measurement. A marker still showing the tuned aim would be comparing
        against the wrong thing."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        p.dbgVals[0][mod.DebugAimSlot:] = [-90.0, 0.0]      # tuned aim: straight down
        p._dbg_apply_aim(0)
        p._dbg_set_probe(7)                                  # probe says +z
        tip = p._dbg_marker_offset(0, mod.DebugMarkerAxisDots)
        self.assertEqual([round(v, 6) for v in tip],
                         [0.0, 0.0, round(p.dbgMarkerReach, 6)])

    def test_off_restores_normal_tuning(self):
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        before = p._dbg_params(0)
        p._dbg_set_probe(4)
        self.assertNotEqual(p._dbg_params(0), before)
        p._dbg_set_probe(None)
        self.assertEqual(p._dbg_params(0), before)

    def test_entering_a_probe_turns_the_markers_on(self):
        """A probe with no reference to compare against answers half the question."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        self.assertEqual(p.dbgMarkerMode, "off")
        p._dbg_set_probe(6)
        self.assertNotEqual(p.dbgMarkerMode, "off")

    def test_there_is_an_expectation_printed_for_every_slot(self):
        """The probe answers by comparison, so what each slot SHOULD do has to be on
        screen — otherwise reading the result is a memory test."""
        mod, _ = load_plugin_module()
        self.assertEqual(len(mod.DebugProbeExpect), mod.DebugParamCount)
        for text in mod.DebugProbeExpect:
            self.assertTrue(text.strip())


class PositionMarkerTests(unittest.TestCase):
    """Billboard dots on a light's origin and along its aim.

    The problem they solve: a spill light is invisible in mid-air and the bright part
    of the pool it casts is nowhere near where it sits, so placement has been guesswork.
    Everything here is about the arrow saying the truth — a marker that lies is worse
    than none, because it would be believed."""

    def test_dot_zero_sits_exactly_on_the_light(self):
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        p.dbgVals[0][mod.DebugPosSlot:] = [0.3, -0.2, 0.1]
        self.assertEqual([round(v, 6) for v in p._dbg_marker_offset(0, 0)],
                         [round(v, 6) for v in p._dbg_pos_offset(0)])

    def test_the_axis_dots_march_evenly_out_to_the_reach(self):
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp, lights=[
            {"n": 0, "name": "a", "fixture": "fx", "category": "dome",
             "source": "panel", "index": 0, "mount": None, "pos": [0.0, 0.0, 0.0],
             "rgb": [1, 1, 1], "size": 1.0, "dir": [0, 0, 1], "cone": 0.9}])
        for dot in range(1, mod.DebugMarkerAxisDots + 1):
            off = p._dbg_marker_offset(0, dot)
            want = p.dbgMarkerReach * dot / mod.DebugMarkerAxisDots
            self.assertEqual([round(v, 6) for v in off],
                             [0.0, 0.0, round(want, 6)])
        # ...and the last axis dot lands exactly ON the reach, which is where the
        # cone ring is drawn — the two must coincide or the marker reads as a cone
        # floating in front of an arrow that stops short.
        self.assertAlmostEqual(p._dbg_marker_offset(0, mod.DebugMarkerAxisDots)[2],
                               p.dbgMarkerReach, places=6)

    def test_the_rim_dots_draw_the_actual_cone(self):
        """The rim is the part that makes aim judgeable. Radius must be
        tan(half-spread) * reach, or the ring shows a beam the light does not have —
        worse than no ring, because it would be believed."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp, lights=[
            {"n": 0, "name": "a", "fixture": "fx", "category": "dome",
             "source": "panel", "index": 0, "mount": None, "pos": [0.0, 0.0, 0.0],
             "rgb": [1, 1, 1], "size": 1.0, "dir": [0, 0, 1], "cone": 0.9}])
        half = math.radians(mod._cone_to_spread(0.9))
        want_r = math.tan(half) * p.dbgMarkerReach
        first = mod.DebugMarkerAxisDots + 1
        seen = []
        for dot in range(first, mod.DebugMarkerDots):
            x, y, z = p._dbg_marker_offset(0, dot)
            # on the plane at `reach` along the axis...
            self.assertAlmostEqual(z, p.dbgMarkerReach, places=6)
            # ...and at the cone radius from it
            self.assertAlmostEqual(math.hypot(x, y), want_r, places=6)
            seen.append((round(x, 4), round(y, 4)))
        self.assertEqual(len(set(seen)), mod.DebugMarkerRimDots,
                         "rim dots must be spread around the ring, not stacked")

    def test_the_rim_survives_a_light_aimed_along_any_axis(self):
        """The perpendicular basis is built with a cross product, which collapses if
        its seed axis is parallel to the aim. Straight down and straight aft are the
        two commonest cockpit aims, so a fixed seed would fail on real data."""
        mod, xp = load_plugin_module()
        for d in ([0, -1, 0], [0, 1, 0], [0, 0, 1], [0, 0, -1], [1, 0, 0],
                  [-1, 0, 0], [0.577, -0.577, 0.577]):
            p = _loaded(mod, xp, lights=[
                {"n": 0, "name": "a", "fixture": "fx", "category": "dome",
                 "source": "panel", "index": 0, "mount": None, "pos": [0.0, 0.0, 0.0],
                 "rgb": [1, 1, 1], "size": 1.0, "dir": d, "cone": 0.9}])
            axis, u, w = mod._perp_basis(d)
            for vec in (axis, u, w):
                self.assertAlmostEqual(math.sqrt(sum(v * v for v in vec)), 1.0,
                                       places=6, msg="basis not unit for %s" % d)
            for a, b in ((axis, u), (axis, w), (u, w)):
                self.assertAlmostEqual(sum(x * y for x, y in zip(a, b)), 0.0,
                                       places=6, msg="basis not orthogonal for %s" % d)

    def test_a_short_dir_vector_still_draws_a_full_length_arrow(self):
        """int_panelflood's `0 -0.15 -0.1` is 0.18 long. Using it unnormalised would
        draw a stub five times shorter than its neighbours' — which the eye reads as a
        weaker light, not as a shorter vector. The arrow shows DIRECTION."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp, lights=[
            {"n": 0, "name": "a", "fixture": "fx", "category": "mainpnl",
             "source": "panel", "index": 1, "mount": None, "pos": [0.0, 0.0, 0.0],
             "rgb": [1, 1, 1], "size": 1.0, "dir": [0, -0.15, -0.1], "cone": 0.9}])
        off = p._dbg_marker_offset(0, mod.DebugMarkerAxisDots)
        self.assertAlmostEqual(math.sqrt(sum(v * v for v in off)),
                               p.dbgMarkerReach, places=6)

    def test_an_omnidirectional_light_has_no_tail_to_draw(self):
        """dir 0 0 0 is omni (the dome). Normalising it would divide by zero; the dots
        stack on the origin instead, which reads as "no direction" rather than crashing
        the accessor mid-frame."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp, lights=[
            {"n": 0, "name": "dome", "fixture": "fx", "category": "dome",
             "source": "panel", "index": 0, "mount": None, "pos": [0.0, 0.0, 0.0],
             "rgb": [1, 1, 1], "size": 1.0, "dir": [0, 0, 0], "cone": 1.0}])
        self.assertEqual(p._dbg_marker_offset(0, 3), [0.0, 0.0, 0.0])

    def test_the_arrow_shows_the_AUTHORED_aim_not_the_permuted_one(self):
        """The marker is the REFERENCE, so it must NOT carry the orientation cycler.

        The first version read aim through `_dbg_params`, which is where the cycler is
        applied — so `Dir < / >` moved the arrow and the light together and the two
        could never be seen to disagree. That made the only instrument capable of
        exposing an axis mismatch incapable of exposing one, and an axis mismatch is
        exactly what got reported in-sim (2026-07-30: at yaw 180, editing pitch moved
        the light horizontally while the markers moved vertically as asked).

        Split, the search works: arrow = what you asked for, light = what the sim did,
        cycle until they agree."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp, lights=[
            {"n": 0, "name": "a", "fixture": "fx", "category": "dome",
             "source": "panel", "index": 0, "mount": None, "pos": [0.0, 0.0, 0.0],
             "rgb": [1, 1, 1], "size": 1.0, "dir": [0, 0, 1], "cone": 0.9}])
        p.dbgOrientDir = [o[2] for o in mod.DebugOrientations].index("Z Y X")
        # the LIGHT is permuted...
        self.assertEqual([round(v, 6) for v in p._dbg_params(0)[5:8]],
                         [1.0, 0.0, 0.0])
        # ...the ARROW is not, so the divergence is visible in the cockpit.
        off = p._dbg_marker_offset(0, mod.DebugMarkerAxisDots)
        self.assertEqual([round(v, 6) for v in off],
                         [0.0, 0.0, round(p.dbgMarkerReach, 6)])

    def test_this_mode_tracks_the_selection(self):
        """Stored as a MODE, not as sel+1: otherwise Next > would leave the arrow on the
        light you just moved off, which reads as the marker being stuck to the wrong one."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        p._dbg_set_marker("this")
        self.assertEqual(p._dbg_marker_value(), 1)
        p._dbg_select(+1)
        self.assertEqual(p._dbg_marker_value(), 2)

    def test_off_and_all_are_the_two_reserved_values(self):
        """The OBJ gates each light on [-0.5, n+0.5] and [n+1.5, big], so 0 falls inside
        the first range for every light (all hidden) and -1 escapes both (all shown).
        Those two are what make one dataref serve every light."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        p._dbg_set_marker("off")
        self.assertEqual(p._dbg_marker_value(), 0)
        p._dbg_set_marker("all")
        self.assertEqual(p._dbg_marker_value(), -1)
        for n in range(len(p.dbgLights)):
            self.assertTrue(-0.5 <= 0 <= n + 0.5, "0 must hide light %d" % n)
            self.assertFalse(-0.5 <= -1 <= n + 0.5, "-1 must show light %d" % n)
            self.assertFalse(n + 1.5 <= -1, "-1 must show light %d" % n)

    def test_the_gate_value_picks_out_exactly_one_light(self):
        """Walk the OBJ's own hide ranges for every (shown light, gate value) pair —
        binding a marker to the wrong light is invisible in-sim, because a marker on the
        neighbour looks exactly like a light that is somewhere unexpected."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        count = 8
        for value in range(1, count + 1):
            shown = [n for n in range(count)
                     if not (-0.5 <= value <= n + 0.5) and not (n + 1.5 <= value)]
            self.assertEqual(shown, [value - 1])

    def test_marker_params_write_all_nine_slots(self):
        """Whether X-Plane pre-fills a custom light's param buffer is still an open
        question here; a marker drawing black because of it would be read as the feature
        not working. So the accessor never relies on the baked values."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        out = []
        self.assertEqual(p.DbgReadMarkerParam(0, out, 0, -1), mod.DebugParamCount)
        self.assertEqual(len(out), mod.DebugParamCount)
        self.assertEqual(out[:4], list(mod.DebugMarkerOriginColor))
        self.assertEqual(out[4], p.dbgMarkerSize)
        # One colour per role, all three visibly different: the marker is only useful
        # if you can tell the light from its aim from its beam edge at a glance.
        seen = []
        for role in range(mod.DebugMarkerRoles):
            got = []
            p.DbgReadMarkerParam(role, got, 0, -1)
            seen.append(tuple(got[:3]))
        self.assertEqual(len(set(seen)), mod.DebugMarkerRoles, seen)
        self.assertEqual(seen[1], tuple(mod.DebugMarkerAxisColor[:3]))
        self.assertEqual(seen[2], tuple(mod.DebugMarkerRimColor[:3]))

    def test_the_offset_array_sizes_and_strides_like_the_obj(self):
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        total = mod.DebugMaxLights * mod.DebugMarkerDots * mod.DebugPosCount
        self.assertEqual(p.DbgReadMarker(0, None, 0, -1), total)
        # light 1, dot 2, axis z -> ((1 * dots) + 2) * 3 + 2
        p.dbgVals[1][mod.DebugPosSlot:] = [0.0, 0.0, 0.0]
        idx = ((1 * mod.DebugMarkerDots) + 2) * mod.DebugPosCount + 2
        out = []
        self.assertEqual(p.DbgReadMarker(0, out, idx, 1), 1)
        self.assertEqual([round(v, 6) for v in out],
                         [round(p._dbg_marker_offset(1, 2)[2], 6)])

    def test_the_stride_comes_from_the_manifest_not_the_constant(self):
        """A debug OBJ baked its own dot count into its indices. Reading the constant
        instead would address a neighbouring light's dots the moment the count changes,
        and every arrow would land on the wrong lamp."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        p.dbgMarkerDots = 3
        seen = []
        p._dbg_marker_offset = lambda n, dot: (seen.append((n, dot)) or [0.0, 0.0, 0.0])
        p.DbgReadMarker(0, [], 0, 3 * 3 * mod.DebugPosCount)
        self.assertEqual(seen[0], (0, 0))
        self.assertEqual(seen[3 * mod.DebugPosCount], (1, 0))

    def test_generator_and_plugin_agree_on_every_marker_constant(self):
        """Four numbers span the two files and each fails silently: a dot-count mismatch
        drives a neighbour's marker, a dataref rename draws nothing at all."""
        mod, _ = load_plugin_module()
        sys.path.insert(0, str(REPO / "build"))
        import build_objs as B
        self.assertEqual(mod.DebugMarkerDots, B.DEBUG_MARK_DOTS)
        self.assertEqual(mod.DebugMarkerDataRef, B.DEBUG_MARK_DREF)
        self.assertEqual(mod.DebugMarkerShowDataRef, B.DEBUG_MARK_SHOW_DREF)
        self.assertEqual(mod.DebugMarkerParamDataRef, B.DEBUG_MARK_PARAM_DREF)
        self.assertEqual(tuple(mod.DebugMarkerST), tuple(B.DEBUG_MARK_ST))


class StepLadderTests(unittest.TestCase):
    """The -/+ buttons move by whatever the lit Step button says, one per decade.

    This replaced a Fine/Coarse pair whose amount ALSO varied per row kind, so the
    distance a click moved something was two hidden variables deep and never on
    screen. The contract now is exactly "the label is the amount"."""

    def test_the_ladder_is_the_decades_the_labels_promise(self):
        """10 was added 2026-07-30 for the angle rows: swinging an aim through a
        quadrant at 1 degree a click is 90 clicks."""
        mod, _ = load_plugin_module()
        self.assertEqual(mod.DebugSteps, (10.0, 1.0, 0.1, 0.01, 0.001))
        self.assertEqual([mod._step_label(s) for s in mod.DebugSteps],
                         ["10", "1", "0.1", "0.01", "0.001"])
        self.assertIn(mod.DebugDefaultStep, mod.DebugSteps)
        # Strictly descending, so the row reads as a ladder rather than a set.
        self.assertEqual(list(mod.DebugSteps), sorted(mod.DebugSteps, reverse=True))

    def test_a_nudge_moves_by_exactly_the_selected_step(self):
        """Every row kind, one step: the point of the change is that the amount no
        longer depends on which row you are on."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        for step in mod.DebugSteps:
            for slot, kind in ((5, "dir"), (4, "size"), (mod.DebugPosSlot, "pos")):
                p.dbgStep = step
                before = p.dbgVals[0][slot]
                p._dbg_nudge(slot, kind, +1)
                self.assertAlmostEqual(p.dbgVals[0][slot] - before, step, places=9)
                p._dbg_nudge(slot, kind, -1)
                self.assertAlmostEqual(p.dbgVals[0][slot], before, places=9)

    def test_repeated_nudges_do_not_accumulate_binary_dust(self):
        """0.1 added ten times is 0.9999999999999999 in IEEE754. Those digits end up
        in the window and, via Log DSL, in the .phdsl — so the nudge rounds."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        p.dbgStep = 0.1
        p.dbgVals[0][mod.DebugPosSlot] = 0.0
        for _ in range(10):
            p._dbg_nudge(mod.DebugPosSlot, "pos", +1)
        self.assertEqual(p.dbgVals[0][mod.DebugPosSlot], 1.0)

    def test_clamping_still_applies_per_kind(self):
        """The step is uniform; the LIMITS are not. rgb/alpha stay in 0..1 or the value
        stops meaning what its row says."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        p.dbgStep = 1.0
        p._dbg_nudge(0, "rgb", +1)
        self.assertEqual(p.dbgVals[0][0], 1.0)
        p._dbg_nudge(3, "alpha", -1)
        self.assertEqual(p.dbgVals[0][3], 0.0)


class PositionOptInTests(unittest.TestCase):
    """The X/Y/Z half of the tool exists only when the OBJ carries the ANIM_trans
    wrappers — everything but a `--no-debug-pos` build. A knob that moves nothing is
    worse than no knob: in a cockpit it reads as the light refusing to move, not as
    the feature being switched off."""

    def test_position_rows_are_hidden_without_the_wrappers(self):
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp, pos_tunable=False)
        kinds = [k for _, _, k in p._dbg_slots()]
        self.assertNotIn("pos", kinds)
        self.assertEqual(len(p._dbg_slots()), len(mod.PythonInterface.DEBUG_SLOTS))

    def test_position_rows_appear_with_them(self):
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp, pos_tunable=True)
        self.assertEqual([k for _, _, k in p._dbg_slots()][-3:], ["pos"] * 3)

    def test_offsets_read_zero_when_the_wrappers_are_absent(self):
        """Nothing reads the array then, so a non-zero value would only make the window
        disagree with the sim about where the light is."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp, pos_tunable=False)
        p.dbgVals[0][mod.DebugPosSlot:] = [0.5, 0.5, 0.5]
        p.dbgOrientPos = 7
        self.assertEqual(p._dbg_pos_offset(0), [0.0, 0.0, 0.0])
        out = []
        p.DbgReadPos(0, out, 0, -1)
        self.assertEqual(set(out), {0.0})

    def test_absolute_position_is_just_the_baked_one_when_hidden(self):
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp, pos_tunable=False)
        p.dbgVals[1][mod.DebugPosSlot:] = [0.25, -0.5, 1.0]
        self.assertEqual([round(x, 3) for x in p._dbg_abs_pos(1)], [1.0, 0.0, 0.0])

    def test_dir_cycling_still_works_and_does_not_drag_position_along(self):
        """Aim goes through the light's own dataref and never needed ANIM_trans, so it
        stays tunable. Link must not silently advance a position hypothesis nothing can
        apply — that would misreport which permutation was under test."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp, pos_tunable=False)
        p.dbgOrientLink = True
        p._dbg_cycle_orient("dir", +1)
        self.assertEqual((p.dbgOrientPos, p.dbgOrientDir), (0, 1))

    def test_the_manifest_flag_is_what_decides(self):
        """Read from the manifest, defaulting False — manifest and OBJ are written
        together, so a missing key means an old or hand-made file and the safe reading
        is 'no wrappers'."""
        mod, xp = load_plugin_module()
        p = mod.PythonInterface()
        p.XPluginStart()
        p.dbgPosTunable = True
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "objects")
            os.makedirs(path)
            with open(os.path.join(path, "lights_inn.debug.json"), "w") as f:
                json.dump({"version": 1, "lights": []}, f)
            p._aircraft_dir_raw = lambda: d
            p._dbg_load()
        self.assertFalse(p.dbgPosTunable)

    def test_generator_writes_the_flag_the_plugin_reads(self):
        """The two sides of one contract — a rename on either would leave the window
        permanently showing the wrong half of itself."""
        sys.path.insert(0, str(REPO / "build"))
        import build_objs as B
        src = (REPO / "build" / "build_objs.py").read_text(encoding="utf-8")
        self.assertIn('"pos_tunable": em.debug_pos', src)
        dev = (REPO / "src" / "plugin" / "PI_PhotonDevReload.py").read_text(
            encoding="utf-8", errors="replace")
        self.assertIn('data.get("pos_tunable", False)', dev)


class OrientationCyclerTests(unittest.TestCase):
    """The 48-hypothesis axis search. Its value is that clicking through it is a
    reliable experiment, so the enumeration must be complete and the identity a genuine
    no-op: a cycler that skipped the right answer, or moved lights at index 0, would
    send the search off after a wrong conclusion."""

    def test_all_48_signed_axis_permutations_are_present_and_distinct(self):
        mod, _ = load_plugin_module()
        self.assertEqual(len(mod.DebugOrientations), 48)
        labels = [o[2] for o in mod.DebugOrientations]
        self.assertEqual(len(set(labels)), 48)

    def test_index_zero_is_the_identity(self):
        """A fresh session must behave exactly as it did before the cycler existed."""
        mod, _ = load_plugin_module()
        self.assertEqual(mod.DebugOrientations[0][2], "X Y Z")
        self.assertEqual(mod._orient(0, [1.5, -2.0, 0.25]), [1.5, -2.0, 0.25])

    def test_orient_permutes_and_signs_the_way_the_label_reads(self):
        """The label is what gets reported back as the answer, so it has to describe
        the transform actually applied, not merely correlate with it."""
        mod, _ = load_plugin_module()
        for perm, signs, label in mod.DebugOrientations:
            out = mod._orient(mod.DebugOrientations.index((perm, signs, label)),
                              [1.0, 2.0, 3.0])
            want = [signs[i] * [1.0, 2.0, 3.0][perm[i]] for i in range(3)]
            self.assertEqual(out, want, label)

    def test_identity_leaves_every_position_offset_at_zero(self):
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        self.assertEqual(p._dbg_pos_offset(1), [0.0, 0.0, 0.0])

    def test_a_permutation_relocates_the_light_to_the_permuted_coordinate(self):
        """The OBJ bakes `pos` and we cannot rewrite it, so the correction has to be
        `permute(pos) - pos`. If it were just `permute(pos)` the light would land at
        double the coordinate and every hypothesis would look wrong."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp, lights=[
            {"n": 0, "name": "a", "fixture": "fx", "category": "dome",
             "source": "panel", "index": 0, "mount": None, "pos": [0.5, 1.0, -4.0],
             "rgb": [1, 1, 1], "size": 1.0, "dir": [0, -1, 0], "cone": 0.9}])
        idx = [o[2] for o in mod.DebugOrientations].index("X Z Y")   # swap y and z
        p.dbgOrientPos = idx
        self.assertEqual([round(x, 3) for x in p._dbg_abs_pos(0)], [0.5, -4.0, 1.0])

    def test_manual_nudges_stay_in_plain_obj_axes_and_still_add(self):
        """Permuting the manual nudge too would make "X +" move along some other axis
        mid-search, which is precisely when you need the two effects separable."""
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp, lights=[
            {"n": 0, "name": "a", "fixture": "fx", "category": "dome",
             "source": "panel", "index": 0, "mount": None, "pos": [1.0, 2.0, 3.0],
             "rgb": [1, 1, 1], "size": 1.0, "dir": [0, -1, 0], "cone": 0.9}])
        p.dbgOrientPos = [o[2] for o in mod.DebugOrientations].index("Y X Z")
        p.dbgVals[0][mod.DebugPosSlot] = 0.1
        self.assertEqual([round(x, 3) for x in p._dbg_abs_pos(0)], [2.1, 1.0, 3.0])

    def test_direction_is_permuted_on_the_way_out(self):
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        p.dbgOrientDir = [o[2] for o in mod.DebugOrientations].index("X -Z Y")
        # light 0's dir is 0 -1 0 -> x stays, y takes -z (0), z takes y (-1)
        self.assertEqual([round(x, 3) for x in p._dbg_params(0)[5:8]], [0.0, 0.0, -1.0])

    def test_linked_cycling_steps_both_axes_together(self):
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        p.dbgOrientLink = True
        p._dbg_cycle_orient("pos", +1)
        self.assertEqual((p.dbgOrientPos, p.dbgOrientDir), (1, 1))
        p.dbgOrientLink = False
        p._dbg_cycle_orient("pos", +1)
        self.assertEqual((p.dbgOrientPos, p.dbgOrientDir), (2, 1))

    def test_cycling_wraps_so_the_sweep_never_dead_ends(self):
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        p.dbgOrientLink = False
        p._dbg_cycle_orient("pos", -1)
        self.assertEqual(p.dbgOrientPos, len(mod.DebugOrientations) - 1)

    def test_the_correction_fits_inside_the_keyframe_span(self):
        """A permutation of a cockpit coordinate is metres, not centimetres. If the
        ANIM_trans span cannot express it X-Plane clamps, and a CORRECT hypothesis
        would show up as a wrong one — the one failure this search cannot survive."""
        mod, _ = load_plugin_module()
        worst = 0.0
        for pos in ([0.9, 1.2, -5.2], [-0.9, 0.2, -4.6], [0.244, 1.09, -3.35]):
            for idx in range(len(mod.DebugOrientations)):
                moved = mod._orient(idx, pos)
                worst = max(worst, max(abs(moved[i] - pos[i]) for i in range(3)))
        self.assertLess(worst, mod.DebugPosRange)

    def test_reset_returns_to_the_identity(self):
        mod, xp = load_plugin_module()
        p = _loaded(mod, xp)
        p.dbgOrientPos, p.dbgOrientDir = 7, 19
        p._dbg_reset_orient()
        self.assertEqual((p.dbgOrientPos, p.dbgOrientDir), (0, 0))


class LazyManifestLoadTests(unittest.TestCase):
    """The manifest has to be read on FIRST ACCESS, not on window-open.

    Accessors register at XPluginStart, when no aircraft exists and so no manifest can
    be read, and the window that used to trigger the read might never be opened. That
    left every light reading zeros from the first frame — reported in-sim 2026-07-28 as
    "everything is zeroed out by default"."""

    def _with_manifest(self, mod, xp, tmp):
        import json
        objects = tmp / "objects"
        objects.mkdir(parents=True)
        (objects / "lights_inn.debug.json").write_text(json.dumps({
            "version": 1, "airframe": "a320", "obj": "lights_inn.obj",
            "lights": [{"n": 0, "name": "a", "fixture": "fx", "category": "dome",
                        "source": "panel", "index": 0, "mount": None,
                        "pos": [0, 1, 2], "rgb": [0.9, 0.8, 0.7], "size": 1.5,
                        "dir": [0, -1, 0], "cone": 0.8}]}), encoding="utf-8")
        xp.datarefs[mod.DebugPanelRheostat] = StubDataRef(
            mod.DebugPanelRheostat, [1.0, 0.0, 0.0, 0.0])

    def test_first_param_read_loads_the_manifest(self):
        import tempfile
        mod, xp = load_plugin_module()
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            self._with_manifest(mod, xp, tmp)
            p = mod.PythonInterface()
            p.XPluginStart()
            p._aircraft_dir_raw = lambda: str(tmp)
            self.assertEqual(p.dbgVals, [])          # nothing loaded yet
            params = p._dbg_params(0)                # ...the sim asks for light 0
            self.assertEqual([round(x, 3) for x in params[:3]], [0.9, 0.8, 0.7])
            self.assertEqual(params[4], 1.5)
            self.assertEqual(params[8], 0.8)

    def test_a_missing_manifest_is_tried_once_not_every_frame(self):
        """The normal case is no debug OBJ installed. That must not cost a filesystem
        probe per light per frame."""
        mod, xp = load_plugin_module()
        p = mod.PythonInterface()
        p.XPluginStart()
        calls = []
        real = p._dbg_load
        p._dbg_load = lambda: (calls.append(1), real())[1]
        for _ in range(10):
            p._dbg_params(0)
            p.DbgReadPos(0, [], 0, 3)
        self.assertEqual(len(calls), 1)

    def test_plane_load_rearms_and_reloads(self):
        """A reload may swap the OBJ (and its manifest) underneath us, so the handler
        re-reads unconditionally: the guard that used to be here — only reload if
        something was already loaded — never fired on a cold start."""
        mod, xp = load_plugin_module()
        p = mod.PythonInterface()
        p.XPluginStart()
        calls = []
        p._dbg_load = lambda: calls.append(1)
        p.state = mod.ST_IDLE
        p.XPluginReceiveMessage(0, mod.MsgPlaneLoaded, 0)
        self.assertEqual(len(calls), 1)


if __name__ == "__main__":
    unittest.main()

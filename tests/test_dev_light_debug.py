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
                                + [mod.DebugPosDataRef, mod.DebugCompareDataRef]))

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
        self.assertEqual(len(names), mod.DebugMaxLights + 2)   # + position + compare
        self.assertIn(mod.DebugDataRefPrefix + "0", names)
        self.assertIn(mod.DebugPosDataRef, names)
        self.assertIn(mod.DebugCompareDataRef, names)

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
        v = [light["rgb"][0], light["rgb"][1], light["rgb"][2], 1.0, light["size"],
             light["dir"][0], light["dir"][1], light["dir"][2], light["cone"],
             0.0, 0.0, 0.0]
        p.dbgVals.append(list(v))
        p.dbgBase.append(list(v))
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
        self.assertEqual(out[:3], [1.0, 0.5, 0.25])       # rgb
        self.assertEqual(out[4], 2.0)                      # size
        self.assertEqual(out[8], 0.9)                      # cone

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
        self.assertEqual(out, [2.0, 0.0])                  # size, dir x

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
        p.dbgSel, p.dbgCoarse = 0, True
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


class PositionOptInTests(unittest.TestCase):
    """The X/Y/Z half of the tool only exists when the OBJ was built with --debug-pos.
    A knob that moves nothing is worse than no knob: in a cockpit it reads as the light
    refusing to move, not as the feature being switched off."""

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

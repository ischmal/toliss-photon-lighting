#!/usr/bin/env python3
"""
Bootstrap per-airframe POSITION overrides for A319 / A321 from their frozen
hand-authored Photon OBJs (reference/photon/), so they don't have to be
hand-transcribed.

STATUS: one-shot tool, already run — the A319/A321 overrides it derived are
already in src/lights/lights.layout.phdsl. Its analysis (`--report`) is still valid and
useful, but its JSON output mode predates the DSL and emits schema-3 blocks for
the retired reference/legacy/lights.poc.jsonc; that JSON can no longer be pasted
into the config. Port the emitter to DSL selector syntax before relying on it
again (see docs/dsl.md § "Airframe overrides").

Method: emit A320 with light-path annotations, and parse the target airframe's
OBJ. Each physical light is keyed by (gate_path, class, x-sign, ordinal) — a key
that is stable across airframes because they share the same gate structure and
light order. For every A320 light we look up the matching target light and, if
its rounded position differs, record an override at that light's config path.
Mirror fixtures only need their port (authored) side; starboard is derived.

Usage:
    python build/bootstrap.py --airframe a319          # print override JSON + report
    python build/bootstrap.py --airframe a321 --report # report only

Verify any config change with:  python build/build_objs.py check --airframe a319
"""
from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict

import build_objs as B


def walk_lights(text: str):
    """Yield (path_or_None, gate_path, cls, pos, xsign) per LIGHT_PARAM, in file
    order. `path` comes from a preceding `# @fixture|gi|li|side` annotation."""
    stack, pending = [], None
    for raw in text.replace("\r\n", "\n").split("\n"):
        line = raw.strip()
        if not line:
            continue
        if line.startswith("# @"):
            pending = line[3:].strip()
            continue
        if line.startswith("#"):
            continue
        tok = line.split()
        h = tok[0]
        if h == "ANIM_begin":
            stack.append([])
        elif h == "ANIM_end":
            if stack:
                stack.pop()
        elif h in ("ANIM_hide", "ANIM_show"):
            if stack:
                stack[-1].append((h, B.rnd(tok[1]), B.rnd(tok[2]), tok[3]))
        elif h == "LIGHT_PARAM":
            body = line.split("#", 1)[0].split()
            cls = body[1]
            nums = []
            for v in body[2:]:
                try:
                    nums.append(float(v.replace("cd", "")))
                except ValueError:
                    pass
            if len(nums) < 12:
                continue
            x, y, z = nums[0], nums[1], nums[2]
            gate = tuple(c for frame in stack for c in frame)
            xs = (B.rnd(x) > 0) - (B.rnd(x) < 0)
            yield pending, gate, cls, (B.rnd(x), B.rnd(y), B.rnd(z)), xs
            pending = None


def is_original(gate) -> bool:
    """A light on the ToLissPhoton `1 1` (original) branch — one instance/light."""
    return any(d.startswith("ToLissPhoton/") and lo == 1.0 and hi == 1.0
               for (_h, lo, hi, d) in gate)


def index_by_key(lights):
    """Bucket original-branch lights by (gate, cls); assign ordinals in file
    order. Config fixture order matches OBJ file order within a bucket, so the
    ordinal pairs A320 lights with their target counterparts (incl. left/right
    fixtures that share a gate, and mirror port/starboard which appear port-first)."""
    out, counter = {}, defaultdict(int)
    for path, gate, cls, pos, xs in lights:
        if not is_original(gate):
            continue
        bkey = (gate, cls)
        ordn = counter[bkey]
        counter[bkey] += 1
        out[(gate, cls, ordn)] = (path, pos)
    return out


# Structural differences that aren't just positions (mounts don't affect the
# check, but matter for a correct build; landing_spill is absent on the A319).
TWEAKS = {
    # A319: beacon + logo placed directly (no mount); its stowed-landing set is a
    # small 4-light block (not A320's 73-light spill), captured as its own raw.
    "a319": {"top_beacon": {"gating": {"mount": None}}, "logo": {"gating": {"mount": None}},
             "landing_spill": {"raw": "src/lights/raw/a319/landing_spill.obj"}},
    # A321: tail rides its own fuselage-translation mount.
    "a321": {"tail_nav": {"gating": {"mount": "tail_fus"}},
             "tail_strobe": {"gating": {"mount": "tail_fus"}}},
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--airframe", required=True, choices=["a319", "a321"])
    ap.add_argument("--report", action="store_true", help="report only, no JSON")
    args = ap.parse_args()

    cfg = B.load_config()
    # A320 annotated -> map key -> (config-path, a320 position)
    a320_text = B.Emitter(cfg, "a320", annotate=True).emit()
    a320 = index_by_key(walk_lights(a320_text))
    # target airframe OBJ
    ref_text, ref_src = B.get_reference(args.airframe)
    tgt = index_by_key(walk_lights(ref_text))

    a320_fx = B.resolve_airframe(cfg, "a320")["fixtures"]
    # overrides[fixture][gi][li] = new position ; None gi means top-level lights
    changed = defaultdict(dict)
    matched = moved = missing = 0
    for key, (path, a_pos) in a320.items():
        if path is None or path.endswith("|starboard"):
            continue
        if key not in tgt:
            missing += 1
            continue
        _, t_pos = tgt[key]
        matched += 1
        if t_pos != a_pos:
            moved += 1
            fixture, gi, li, _side = path.split("|")
            changed[fixture][(int(gi), int(li))] = list(t_pos)

    extra = sum(1 for k, (p, _) in tgt.items() if k not in a320)
    print(f"# bootstrap {args.airframe}: reference {ref_src}", file=sys.stderr)
    print(f"#   matched {matched}, moved {moved}, a320-only {missing}, "
          f"target-only {extra}", file=sys.stderr)

    # Build override skeletons that align with each fixture's group/light shape.
    fixtures_out = {}
    for fixture, moves in changed.items():
        fx = a320_fx[fixture]
        groups = fx["groups"] if "groups" in fx else [{"lights": fx["lights"]}]
        g_out = []
        any_group = "groups" in fx
        for gi, g in enumerate(groups):
            lights_out = []
            for li in range(len(g["lights"])):
                lights_out.append({"position": moves[(gi, li)]} if (gi, li) in moves else {})
            g_out.append({"lights": lights_out})
        fixtures_out[fixture] = {"groups": g_out} if any_group else g_out[0]

    # Merge in non-positional structural tweaks (mounts / removals).
    for fx, patch in TWEAKS.get(args.airframe, {}).items():
        if patch is None:
            fixtures_out[fx] = None
        else:
            fixtures_out[fx] = B.deep_merge(fixtures_out.get(fx, {}), patch)

    if not args.report:
        print(json.dumps({"extends": "a320", "fixtures": fixtures_out},
                         indent=2))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
ToLiss Photon Lighting — DSL loader (Phase II).

Parses the CSS-like light config (src/lights/lights.style.phdsl +
src/lights/lights.layout.phdsl)
into the same schema-3 dict that the retired lights.poc.jsonc decoded to, so
build_objs.py's Emitter consumes either source unchanged. Full spec in docs/dsl.md.

Format at a glance:

    axis profile: halogen xenon led;              // declared variant axes
    palette xenon { white: 0.75 0.75 1;  @bb { white: 0.45 0.45 1; } }
    light nav_beam extends nav_base {
        class: airplane_nav_bb;  cone: 0.2;
        intensity: 5000;  @starboard { intensity: 4000; }
        @led { intensity: 6000;  @starboard { intensity: 5000; } }
    }
    airframe a320 {
        fixture tail_nav "Tail Nav" {
            category: nav;
            group g1 "Light 1" {
                hide: 1.5 2.5 AirbusFBW/OHPLightSwitches[2];
                pos: -0.07 0.7 31.26;
                nav_tail_bb;
                nav_tail_sp;
            }
        }
    }
    airframe a319 extends a320 {
        tail_nav { g1 nav_tail_bb { pos: -0.042 0.706 29.14; } }  // selector override
    }

Resolution rule (deliberately NOT the CSS cascade): for a concrete context
(profile, side, swatch, wingtip, wing), the applicable declaration with the MOST
conditions wins; two different values at equal specificity are an error. A later
declaration with the IDENTICAL condition set replaces an earlier one — that is
what `extends` (light mixins and airframe overrides) relies on.

Axes are free or bound. profile/side/swatch are free: one OBJ carries every
profile branch and both sides, so they expand into nested dicts the Emitter
resolves later (AXIS_ORDER). `wing` is bound (BOUND_AXES): --wing fixes it for
the whole build, so it is passed in here as a base context and its conditions
merely filter — the result is a plain scalar, specific to that variant, and the
config is tagged "wing" so a mismatched Emitter is caught rather than silently
emitting another variant's values.
"""
from __future__ import annotations

import copy


class DslError(Exception):
    pass


MISSING = object()

# canonical nesting order when re-encoding conditional values into the
# schema-3 nested-dict shapes resolve_field / pick_side already understand.
# `wingtip` (fence/sharklet) is free but appears ONLY on the RealWings @realwings
# offset, which the emitter never resolves (those fixtures are omit: realwings) —
# patch_realwings.py resolves it. Placing it before `side` nests it wingtip-major.
#
# `intprofile` (int_old/int_new/int_led) is the INTERIOR's own profile axis,
# kept separate from `profile` on purpose: the two never appear on the same
# light, so a separate axis means an interior @int_led block does not expand
# into a halogen/xenon/led cross product (and, more importantly, exterior
# resolution is left completely untouched by the interior work).
AXIS_ORDER = ("profile", "intprofile", "swatch", "wingtip", "side")

# Axes NOT in AXIS_ORDER are *pre-bound*: fixed for a whole build, so they are
# supplied as a base context at load time and act as a filter on @-conditions
# rather than as a dimension of the expanded value. `wing` is pre-bound because
# --wing picks one variant per build, so it never has to survive into the OBJ:
# everything collapses to a scalar before the Emitter sees it, which is what
# keeps resolve_field (and the rest of the Emitter) out of this entirely.
BOUND_AXES = ("wing",)

# DSL property name -> schema-3 key
PROP_MAP = {"pos": "position"}


# ─── tokenizer ────────────────────────────────────────────────────────────────
def tokenize(text: str, filename: str = "?"):
    """Tokens: ('word', text) | ('str', text) | one of '{' '}' ':' ';' '@'.
    Words may contain / [ ] # . - etc (datarefs, paths, labels); // and /* */
    comments are skipped."""
    toks = []
    i, n, line = 0, len(text), 1
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
            continue
        if c in " \t\r":
            i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                if text[i] == "\n":
                    line += 1
                i += 1
            if i + 1 >= n:
                raise DslError(f"{filename}:{line}: unterminated block comment")
            i += 2
            continue
        if c == '"':
            j = i + 1
            while j < n and text[j] not in '"\n':
                j += 1
            if j >= n or text[j] != '"':
                raise DslError(f"{filename}:{line}: unterminated string")
            toks.append(("str", text[i + 1:j], line))
            i = j + 1
            continue
        if c in "{}:;@":
            toks.append((c, c, line))
            i += 1
            continue
        j = i
        while j < n:
            cj = text[j]
            if cj in ' \t\r\n{}:;@"':
                break
            if cj == "/" and j + 1 < n and text[j + 1] in "/*":
                break
            j += 1
        toks.append(("word", text[i:j], line))
        i = j
    return toks


def _val(tok):
    """Convert a value token: int/float when numeric, true/false/none keywords,
    otherwise the raw string (color names, datarefs, paths)."""
    kind, v, _ = tok
    if kind == "str":
        return v
    if v == "true":
        return True
    if v == "false":
        return False
    if v == "none":
        return None
    try:
        return int(v)
    except ValueError:
        pass
    try:
        return float(v)
    except ValueError:
        return v


# ─── parser (generic AST) ─────────────────────────────────────────────────────
class Stmt:
    """kind: 'decl' (words: value...;) | 'block' (words "str"? { children }) |
    'ref' (word;). Condition heads are stored as a single '@value' word."""
    __slots__ = ("kind", "words", "string", "values", "children", "line")

    def __init__(self, kind, words, line, string=None, values=None, children=None):
        self.kind = kind
        self.words = words
        self.line = line
        self.string = string
        self.values = values
        self.children = children


class Parser:
    def __init__(self, toks, filename: str = "?"):
        self.toks = toks
        self.i = 0
        self.filename = filename

    def _peek(self):
        if self.i < len(self.toks):
            return self.toks[self.i]
        last = self.toks[-1][2] if self.toks else 0
        return (None, None, last)

    def _next(self):
        t = self._peek()
        self.i += 1
        return t

    def _err(self, msg, line):
        raise DslError(f"{self.filename}:{line}: {msg}")

    def parse_file(self):
        out = []
        while self.i < len(self.toks):
            if self._peek()[0] == "}":
                self._err("unmatched '}'", self._peek()[2])
            out.append(self.parse_stmt())
        return out

    def parse_stmt(self):
        words, string = [], None
        line = self._peek()[2]
        while True:
            kind, v, ln = self._peek()
            if kind == "word":
                words.append(v)
                self._next()
            elif kind == "str":
                if string is not None:
                    self._err("multiple strings in one statement head", ln)
                string = v
                self._next()
            elif kind == "@":
                self._next()
                k2, v2, l2 = self._next()
                if k2 != "word":
                    self._err("expected an axis value after '@'", l2)
                words.append("@" + v2)
            elif kind == ":":
                if not words:
                    self._err("declaration has no name", ln)
                self._next()
                vals = []
                while True:
                    k3, _, l3 = self._peek()
                    if k3 == ";":
                        self._next()
                        break
                    if k3 in (None, "{", "}", ":", "@"):
                        self._err("expected ';' to end the declaration", l3)
                    vals.append(_val(self._next()))
                if not vals:
                    self._err("declaration has no value", ln)
                return Stmt("decl", words, line, values=vals)
            elif kind == "{":
                if not words:
                    self._err("block has no name", ln)
                self._next()
                children = []
                while True:
                    k4, _, l4 = self._peek()
                    if k4 == "}":
                        self._next()
                        break
                    if k4 is None:
                        self._err("unclosed '{'", line)
                    children.append(self.parse_stmt())
                return Stmt("block", words, line, string=string, children=children)
            elif kind == ";":
                self._next()
                if len(words) != 1 or string is not None:
                    self._err("bad statement before ';'", ln)
                return Stmt("ref", words, line)
            elif kind is None:
                self._err("unexpected end of file", line)
            else:
                self._err(f"unexpected {kind!r}", ln)


# ─── conditional-value resolution ─────────────────────────────────────────────
def _decl_value(values):
    return values[0] if len(values) == 1 else list(values)


def collect_decls(children, axis_of, where, conds=frozenset()):
    """Flatten a block body into [(prop, conds, values)]; nested @blocks AND
    their axis conditions together."""
    out = []
    for st in children:
        if st.kind == "decl":
            if len(st.words) != 1:
                raise DslError(f"{where}:{st.line}: bad declaration name "
                               f"{' '.join(st.words)!r}")
            out.append((st.words[0], conds, st.values))
        elif st.kind == "block" and st.words and st.words[0].startswith("@"):
            v = st.words[0][1:]
            ax = axis_of.get(v)
            if ax is None:
                raise DslError(f"{where}:{st.line}: unknown condition @{v} "
                               f"(not a declared axis value)")
            out += collect_decls(st.children, axis_of, where,
                                 conds | frozenset([(ax, v)]))
        else:
            raise DslError(f"{where}:{st.line}: unexpected statement "
                           f"{' '.join(st.words)!r} here")
    return out


def _dedup(decls):
    """Later declaration with an identical condition set replaces the earlier
    one (mixin/override semantics)."""
    dedup = {}
    for conds, v in decls:
        dedup[conds] = v
    return list(dedup.items())


def _cell(items, ctx, where):
    """Most-conditions-wins for one concrete context; two different values at
    the winning specificity are an error (lower-specificity losers are fine)."""
    applicable = [(len(conds), v) for conds, v in items
                  if all(ctx.get(a) == val for a, val in conds)]
    if not applicable:
        return MISSING
    bestn = max(n for n, _ in applicable)
    winners = [v for n, v in applicable if n == bestn]
    for v in winners[1:]:
        if v != winners[0]:
            raise DslError(f"{where}: ambiguous value for {ctx}: "
                           f"{winners[0]!r} vs {v!r} at equal specificity")
    return winners[0]


def expand_value(decls, axes, where, base=None):
    """Expand [(conds, value)] into the schema-3 shape: a scalar/list, or
    nested dicts keyed by profile and/or port/starboard (profile-major).

    `base` pre-binds the BOUND_AXES (e.g. {"wing": "realwings"}): conditions on
    those axes filter declarations in or out here and add specificity, but never
    become a key in the result."""
    base = dict(base or {})
    items = _dedup(decls)
    involved = [a for a in AXIS_ORDER
                if any(ax == a for conds, _ in items for ax, _ in conds)]
    if not involved:
        # NB: not `items[-1][1]` — with nothing in AXIS_ORDER involved there may
        # still be BOUND_AXES conditions to honour, and last-wins would ignore
        # them (silently picking another variant's value).
        result = _cell(items, base, where)
        if result is MISSING:
            raise DslError(f"{where}: no applicable value for any context")
        return result

    def build(cur, remaining):
        if not remaining:
            return _cell(items, cur, where)
        ax, rest = remaining[0], remaining[1:]
        out = {}
        for val in axes[ax]:
            sub = build({**cur, ax: val}, rest)
            if sub is not MISSING:
                out[val] = sub
        if not out:
            return MISSING
        if ax in ("side", "swatch") and len(out) < len(axes[ax]):
            raise DslError(f"{where}: value defined for only {list(out)} of axis "
                           f"'{ax}'; add an unconditioned base")
        vals = list(out.values())
        if len(out) == len(axes[ax]) and all(v == vals[0] for v in vals):
            return vals[0]  # all contexts agree -> collapse
        return out

    result = build(base, involved)
    if result is MISSING:
        raise DslError(f"{where}: no applicable value for any context")
    return result


def expand_palette_color(decls, where, base=None):
    """Palette colors always emit explicit {bb, sp} swatches (the emitter
    indexes them directly)."""
    items = _dedup(decls)
    out = {}
    for swatch in ("bb", "sp"):
        v = _cell(items, {**(base or {}), "swatch": swatch}, where)
        if v is MISSING:
            raise DslError(f"{where}: no value for swatch '{swatch}'")
        out[swatch] = v if isinstance(v, list) else [v]
    return out


def _expand_props(props, axes, where, base=None):
    """props: [(prop, conds, values)] -> {schema-3 key: expanded value}."""
    by = {}
    order = []
    for prop, conds, values in props:
        key = PROP_MAP.get(prop, prop)
        if key not in by:
            by[key] = []
            order.append(key)
        by[key].append((conds, _decl_value(values)))
    return {k: expand_value(by[k], axes, f"{where}.{k}", base) for k in order}


# ─── style file ───────────────────────────────────────────────────────────────
def _load_style(stmts, filename, base=None):
    axes, axis_of = {}, {}
    palettes, categories = {}, {}
    light_raw = {}  # name -> (parent, decls)
    for st in stmts:
        if st.kind == "decl" and st.words[0] == "axis":
            if len(st.words) != 2:
                raise DslError(f"{filename}:{st.line}: usage: axis <name>: v1 v2 ...;")
            name = st.words[1]
            axes[name] = [str(v) for v in st.values]
            for v in axes[name]:
                axis_of[v] = name
        elif st.kind == "block" and st.words[0] == "palette":
            pname = st.words[1]
            where = f"{filename}: palette {pname}"
            per_color, order = {}, []
            for prop, conds, values in collect_decls(st.children, axis_of, where):
                if prop not in per_color:
                    per_color[prop] = []
                    order.append(prop)
                per_color[prop].append((conds, _decl_value(values)))
            palettes[pname] = {c: expand_palette_color(per_color[c], f"{where}.{c}",
                                                      base)
                               for c in order}
        elif st.kind == "block" and st.words[0] == "categories":
            for d in st.children:
                if d.kind != "decl" or len(d.words) != 1:
                    raise DslError(f"{filename}:{d.line}: categories holds "
                                   "'<name>: <palette> [<palette> ...];' lines only")
                # ONE value is the original two-way form and still means
                # "<palette> led" — branch 0 = that palette, branch 1 = led.
                # TWO OR MORE is the explicit N-way form: branch i renders
                # profile i and is gated on the dataref reading i. The interior
                # needs three (old halogen / new halogen / LED); every exterior
                # category stays one-valued so its emission is unchanged.
                vals = [str(v) for v in d.values]
                profiles = [vals[0], "led"] if len(vals) == 1 else vals
                if len(set(profiles)) != len(profiles):
                    raise DslError(f"{filename}:{d.line}: category "
                                   f"{d.words[0]!r} repeats a profile: {profiles}")
                categories[d.words[0]] = {"original": profiles[0],
                                          "profiles": profiles}
        elif st.kind == "block" and st.words[0] == "light":
            name = st.words[1]
            parent = None
            if len(st.words) == 4 and st.words[2] == "extends":
                parent = st.words[3]
            elif len(st.words) != 2:
                raise DslError(f"{filename}:{st.line}: usage: light <name> "
                               "[extends <parent>] {{ ... }}")
            where = f"{filename}: light {name}"
            light_raw[name] = (parent, collect_decls(st.children, axis_of, where))
        else:
            raise DslError(f"{filename}:{st.line}: unknown top-level statement "
                           f"{' '.join(st.words)!r}")

    def type_decls(name, seen=()):
        if name in seen:
            raise DslError(f"{filename}: light {name}: circular 'extends'")
        if name not in light_raw:
            raise DslError(f"{filename}: light extends unknown type {name!r}")
        parent, decls = light_raw[name]
        base = type_decls(parent, seen + (name,)) if parent else []
        return base + decls

    light_types = {}
    for name in light_raw:
        light_types[name] = _expand_props(
            [(p, c, v) for p, c, v in type_decls(name)],
            axes, f"{filename}: light {name}", base)
    return axes, axis_of, palettes, categories, light_types


# ─── doc file: named model ────────────────────────────────────────────────────
def parse_light(st, axis_of, where):
    if st.kind == "block" and len(st.words) != 1:
        raise DslError(f"{where}:{st.line}: unexpected statement "
                       f"{' '.join(st.words)!r}")
    name = st.words[0]
    type_, _, label = name.partition("#")
    props = (collect_decls(st.children, axis_of, where)
             if st.kind == "block" else [])
    return {"kind": "light", "type": type_, "label": label or None, "props": props}


def parse_group(st, axis_of, where):
    g = {"kind": "group", "name": st.words[1] if len(st.words) > 1 else None,
         "comment": st.string, "gate": None, "position": None, "lights": []}
    gwhere = f"{where} group {g['name']}"
    for c in st.children:
        if c.kind == "decl" and c.words == ["hide"]:
            if len(c.values) != 3:
                raise DslError(f"{gwhere}:{c.line}: usage: hide: <lo> <hi> <dataref>;")
            g["gate"] = {"dref": str(c.values[2]), "hide": [c.values[0], c.values[1]]}
        elif c.kind == "decl" and c.words == ["pos"]:
            g["position"] = list(c.values)
        elif c.kind == "decl":
            raise DslError(f"{gwhere}:{c.line}: unknown group property "
                           f"{c.words[0]!r}")
        else:
            g["lights"].append(parse_light(c, axis_of, gwhere))
    return g


def parse_fixture(st, axis_of, where):
    fx = {"kind": "fixture", "comment": st.string,
          "props": [], "groups": [], "lights": []}
    fwhere = f"{where} fixture {st.words[1]}"
    for c in st.children:
        if c.kind == "decl":
            fx["props"].append((c.words[0], frozenset(), c.values))
        elif c.kind == "block" and c.words[0].startswith("@"):
            fx["props"] += collect_decls([c], axis_of, fwhere)
        elif c.kind == "block" and c.words[0] == "group":
            fx["groups"].append(parse_group(c, axis_of, fwhere))
        else:
            fx["lights"].append(parse_light(c, axis_of, fwhere))
    if fx["groups"] and fx["lights"]:
        raise DslError(f"{fwhere}: a fixture holds groups OR direct lights, not both")
    return fx


def _find_light(lights, sel, where):
    if sel.startswith("#"):
        matches = [l for l in lights if l["label"] == sel[1:]]
    else:
        t, _, lab = sel.partition("#")
        matches = [l for l in lights
                   if l["type"] == t and (not lab or l["label"] == lab)]
    if len(matches) != 1:
        raise DslError(f"{where}: light selector {sel!r} matched "
                       f"{len(matches)} lights (need exactly 1)")
    return matches[0]


def _descend(root, node, word, where):
    if node is root:
        if word not in root:
            raise DslError(f"{where}: unknown fixture {word!r}")
        return root[word]
    if node["kind"] == "fixture":
        if node["groups"]:
            for g in node["groups"]:
                if g["name"] == word:
                    return g
            raise DslError(f"{where}: no group named {word!r} "
                           f"(groups: {[g['name'] for g in node['groups']]})")
        return _find_light(node["lights"], word, where)
    if node["kind"] == "group":
        return _find_light(node["lights"], word, where)
    raise DslError(f"{where}: cannot select {word!r} inside a light")


def _apply_decl(node, prop, conds, values, where, line):
    if node["kind"] == "fixture":
        node["props"].append((prop, conds, values))
    elif node["kind"] == "group":
        if prop == "hide":
            node["gate"] = {"dref": str(values[2]), "hide": [values[0], values[1]]}
        elif prop == "pos":
            node["position"] = list(values)
        else:
            raise DslError(f"{where}:{line}: unknown group property {prop!r}")
    else:  # light
        node["props"].append((prop, conds, values))


def apply_override(root, node, st, axis_of, where):
    target = node
    for w in st.words:
        target = _descend(root, target, w, f"{where}:{st.line}")
    if not isinstance(target, dict) or "kind" not in target:
        raise DslError(f"{where}:{st.line}: bad override target")
    for c in st.children:
        if c.kind == "decl":
            if len(c.words) != 1:
                raise DslError(f"{where}:{c.line}: bad declaration name")
            _apply_decl(target, c.words[0], frozenset(), c.values, where, c.line)
        elif c.kind == "block" and c.words[0].startswith("@"):
            for prop, conds, values in collect_decls([c], axis_of, where):
                _apply_decl(target, prop, conds, values, where, c.line)
        elif c.kind == "block":
            apply_override(root, target, c, axis_of, where)
        else:
            raise DslError(f"{where}:{c.line}: unexpected statement in override")


# ─── named model -> schema-3 dicts ────────────────────────────────────────────
def _vec3(v, where):
    ok = (isinstance(v, list) and len(v) == 3
          and all(isinstance(x, (int, float)) and not isinstance(x, bool) for x in v))
    if not ok:
        raise DslError(f"{where}: expected 3 numbers, got {v!r}")
    return v


_OFFSET_AXIS_KEYS = {"port", "starboard", "fence", "sharklet"}


def _offset_value(v, where):
    """A fixture offset is a translation, so it may vary by side (@port/@starboard),
    by wing (@realwings — pre-bound, filtered out before we get here) and by
    wingtip (@fence/@sharklet, RealWings-only, consumed by patch_realwings.py) —
    but not by profile or swatch (one emitted fixture spans every profile branch).
    Surviving side/wingtip conditions come through as a (possibly nested) dict."""
    if isinstance(v, dict):
        extra = set(v) - _OFFSET_AXIS_KEYS
        if extra:
            raise DslError(f"{where}: offset may vary by side (@port/@starboard) "
                           f"and wingtip (@fence/@sharklet) only, not by {sorted(extra)}")
        return {k: _offset_value(x, f"{where}.{k}") for k, x in v.items()}
    return _vec3(v, where)


def light_to_old(l, axes, where, base=None):
    out = {"type": l["type"]}
    out.update(_expand_props(l["props"], axes, f"{where} {l['type']}", base))
    return out


def group_to_old(g, axes, where, base=None):
    out = {}
    if g["name"]:
        out["name"] = g["name"]
    if g["comment"]:
        out["comment"] = g["comment"]
    if g["gate"]:
        out["gate"] = g["gate"]
    if g["position"] is not None:
        out["position"] = g["position"]
    out["lights"] = [light_to_old(l, axes, where, base) for l in g["lights"]]
    return out


def fixture_to_old(fx, axes, where, base=None):
    props = _expand_props(fx["props"], axes, where, base)
    out = {}
    if fx["comment"]:
        out["comment"] = fx["comment"]
    # Which OBJ this fixture belongs in. Defaults to exterior, so every existing
    # fixture is unaffected; `target: interior;` routes a fixture into
    # lights_inn.obj instead. The Emitter builds one target at a time and skips
    # everything else.
    if "target" in props:
        tgt = props["target"]
        if tgt not in ("exterior", "interior"):
            raise DslError(f"{where}: target must be 'exterior' or 'interior', "
                           f"got {tgt!r}")
        out["target"] = tgt
    if "raw" in props:
        if "offset" in props:
            raise DslError(f"{where}: 'offset' is not supported on a raw fixture "
                           "(its block is copied verbatim); move the light into "
                           "the DSL or bake the offset into the raw file")
        out["raw"] = props["raw"]
        return out
    if "category" not in props:
        raise DslError(f"{where}: fixture needs 'category' (or 'raw')")
    gating = {"category": props["category"]}
    if props.get("mirror") is True:
        gating["mirror"] = True
    if "omit" in props:
        v = props["omit"]
        gating["omit_for"] = v if isinstance(v, list) else [v]
    if "mount" in props:
        gating["mount"] = props["mount"]
    out["gating"] = gating
    if "position" in props:
        out["position"] = props["position"]
    if "offset" in props:
        out["offset"] = _offset_value(props["offset"], f"{where}.offset")
    if fx["groups"]:
        out["groups"] = [group_to_old(g, axes, f"{where} group {g['name']}", base)
                         for g in fx["groups"]]
    elif fx["lights"]:
        out["lights"] = [light_to_old(l, axes, where, base) for l in fx["lights"]]
    return out


def _load_doc(stmts, filename, axes, axis_of, base=None):
    mounts, airframe_stmts, order = {}, {}, []
    for st in stmts:
        if st.kind == "block" and st.words[0] == "mounts":
            af = st.words[1]
            m = {}
            for c in st.children:
                if c.kind != "block" or len(c.words) != 1:
                    raise DslError(f"{filename}:{c.line}: mounts holds "
                                   "'<name> {{ <variant>: <path>; }}' blocks")
                m[c.words[0]] = {d.words[0]: str(_decl_value(d.values))
                                 for d in c.children}
            mounts[af] = m
        elif st.kind == "block" and st.words[0] == "airframe":
            name = st.words[1]
            ext = None
            if len(st.words) == 4 and st.words[2] == "extends":
                ext = st.words[3]
            elif len(st.words) != 2:
                raise DslError(f"{filename}:{st.line}: usage: airframe <name> "
                               "[extends <base>] {{ ... }}")
            airframe_stmts[name] = (ext, st)
            order.append(name)
        else:
            raise DslError(f"{filename}:{st.line}: unknown top-level statement "
                           f"{' '.join(st.words)!r}")

    models = {}

    def build_model(name, seen=()):
        if name in models:
            return models[name]
        if name in seen:
            raise DslError(f"{filename}: airframe {name}: circular 'extends'")
        if name not in airframe_stmts:
            raise DslError(f"{filename}: airframe extends unknown {name!r}")
        ext, st = airframe_stmts[name]
        where = f"{filename}: airframe {name}"
        model = copy.deepcopy(build_model(ext, seen + (name,))) if ext else {}
        for c in st.children:
            if c.kind == "block" and c.words[0] == "fixture":
                model[c.words[1]] = parse_fixture(c, axis_of, where)
            elif c.kind == "block" and not c.words[0].startswith("@"):
                apply_override(model, model, c, axis_of, where)
            else:
                raise DslError(f"{where}:{c.line}: unexpected statement "
                               f"{' '.join(c.words)!r}")
        models[name] = model
        return model

    airframes = {}
    for name in order:
        model = build_model(name)
        where = f"{filename}: airframe {name}"
        airframes[name] = {"fixtures": {
            fname: fixture_to_old(fx, axes, f"{where} fixture {fname}", base)
            for fname, fx in model.items()}}

    # flatten mounts down each extends chain so lookups need no chain walk
    merged_mounts = {}
    for name in order:
        chain, cur = [], name
        while cur is not None:
            chain.append(cur)
            cur = airframe_stmts[cur][0]
        m = {}
        for af in reversed(chain):
            m.update(mounts.get(af, {}))
        if m:
            merged_mounts[name] = m
    return merged_mounts, airframes


# ─── entry point ──────────────────────────────────────────────────────────────
def load_dsl(style_path, doc_path, wing: str = "stock") -> dict:
    """Parse the DSL pair for one wing variant. `wing` is pre-bound (see
    BOUND_AXES): @stock/@durantula/@realwings conditions are resolved here, so
    the returned config is specific to that variant — it is tagged with "wing"
    so the Emitter can refuse a mismatched pairing."""
    style_text = style_path.read_text(encoding="utf-8")
    doc_text = doc_path.read_text(encoding="utf-8")
    style = Parser(tokenize(style_text, style_path.name),
                   style_path.name).parse_file()
    doc = Parser(tokenize(doc_text, doc_path.name), doc_path.name).parse_file()
    base = {"wing": wing}
    axes, axis_of, palettes, categories, light_types = _load_style(
        style, style_path.name, base)
    for required in ("profile", "side", "swatch", "wing"):
        if required not in axes:
            raise DslError(f"{style_path.name}: missing 'axis {required}: ...;'")
    if wing not in axes["wing"]:
        raise DslError(f"{style_path.name}: unknown wing variant {wing!r}; "
                       f"'axis wing' declares {axes['wing']}")
    mounts, airframes = _load_doc(doc, doc_path.name, axes, axis_of, base)
    return {
        "schema": 3,
        "wing": wing,
        "axes": axes,        # so the Emitter knows every profile-axis value
        "palettes": palettes,
        "categories": categories,
        "lightTypes": light_types,
        "mounts": mounts,
        "airframes": airframes,
    }

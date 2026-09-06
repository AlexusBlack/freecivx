#!/usr/bin/env python3
"""Generate docs/space/terrain.html from the space ruleset.

Reads data/space/terrain.ruleset and data/space/effects.ruleset, composes the
yields a tile actually produces, and writes a static page that shows each
terrain and celestial body as the space tileset draws it.

Python 3 standard library only. No arguments; run it from anywhere:

    python3 docs/space/gen_terrain.py

The point of generating rather than hand-writing is that the interesting
numbers are not written down anywhere. A body's base yield is in a
[resource_*] section, the ring it sits on contributes its own food, and what
mining or irrigation adds lives in effects.ruleset as an Output_Add_Tile
effect keyed on an extra flag. Only the sum matters to a player, and only
this script computes it.
"""

import html
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RULESET = ROOT / "data" / "space"
OUT = Path(__file__).resolve().parent / "terrain.html"
IMG = Path(__file__).resolve().parent / "img"

TILE_W, TILE_H = 96, 48


def die(msg):
    sys.exit("gen_terrain: " + msg)


# --------------------------------------------------------------------------
# Ruleset parsing
#
# Just enough of freeciv's INI dialect for our purposes: section headers,
# key = value, backslash line continuations, gettext _("...") wrappers, and
# the { ... } table form. Anything this does not understand is skipped, so
# every lookup below is checked and fails loudly rather than defaulting.
# --------------------------------------------------------------------------

def parse(path):
    if not path.exists():
        die("missing ruleset file %s" % path)

    sections, current = {}, None
    lines, buf = [], ""

    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.split(";")[0] if raw.lstrip().startswith(";") else raw
        line = line.rstrip()
        if line.endswith("\\"):
            buf += line[:-1]
            continue
        lines.append(buf + line)
        buf = ""

    in_table = False
    pending = None          # a "key =" whose { table } opens on the next line
    for line in lines:
        stripped = line.strip()
        if not stripped:
            continue
        m = re.match(r"^\[([^\]]+)\]", stripped)
        if m:
            current = m.group(1)
            sections.setdefault(current, {})
            in_table, pending = False, None
            continue
        if current is None:
            continue
        if in_table:
            sections[current]["__table__"].append(stripped)
            if stripped.startswith("}"):
                in_table = False
            continue
        if pending and stripped.startswith("{"):
            sections[current]["__table__"] = [stripped]
            sections[current][pending] = "__table__"
            in_table = not stripped.rstrip().endswith("}")
            pending = None
            continue
        m = re.match(r'^([A-Za-z_][A-Za-z0-9_.]*)\s*=\s*(.*)$', stripped)
        if not m:
            continue
        key, val = m.group(1), m.group(2).strip()
        pending = None
        if val.startswith("{"):
            sections[current]["__table__"] = [val]
            sections[current][key] = "__table__"
            in_table = not val.rstrip().endswith("}")
            continue
        if not val:
            pending = key
            continue
        sections[current][key] = val

    return sections


def unquote(val):
    """_("Foo") and "Foo" both become Foo, with \\n turned into real breaks."""
    val = val.strip()
    out, i = [], 0
    while i < len(val):
        m = re.compile(r'_\(\s*"((?:[^"\\]|\\.)*)"\s*\)|"((?:[^"\\]|\\.)*)"').match(val, i)
        if m:
            out.append(m.group(1) if m.group(1) is not None else m.group(2))
            i = m.end()
        else:
            i += 1
    return "".join(out).replace("\\n", "\n").replace('\\"', '"')


def strlist(val):
    """A comma-separated list of quoted strings."""
    return [s for s in re.findall(r'"((?:[^"\\]|\\.)*)"', val or "")]


def num(sec, key, default=None):
    if key not in sec:
        if default is None:
            die("expected key %r in section" % key)
        return default
    try:
        return int(sec[key].split()[0])
    except ValueError:
        die("key %r is not a number: %r" % (key, sec[key]))


# --------------------------------------------------------------------------
# Sprite atlas
#
# Freeciv sprites sit on a plain grid, so a sprite is a background-position on
# the sheet -- no image tooling needed. The offsets come from client/tilespec.c
# (xr = x_top_left + (dx + pixel_border) * column).
#
# The space tileset is a repainted amplio2: the ruleset binds each space
# terrain to an existing amplio section name through graphic_alt, and the art
# in that slot is space art. Only "star" needed a new tilespec section.
# --------------------------------------------------------------------------

SHEETS = {
    #  file            x0  y0  dx  dy  border
    "terrain1": ("terrain1.png", 1, 1, TILE_W, TILE_H, 1),
    "terrain2": ("terrain2.png", 1, 1, TILE_W, TILE_H, 1),
    "hills":    ("hills.png",    1, 1, TILE_W, TILE_H, 1),
    "ocean":    ("ocean.png",    0, 0, TILE_W, TILE_H, 0),
}


def png_size(path):
    with path.open("rb") as fh:
        head = fh.read(24)
    if head[:8] != b"\x89PNG\r\n\x1a\n" or head[12:16] != b"IHDR":
        die("%s is not a PNG" % path)
    return struct.unpack(">II", head[16:24])


SHEET_SIZE = {}


def sprite(sheet, row, col):
    """CSS for one 96x48 cell, with a bounds check against the real PNG.

    Worth checking: terrain1.spec's row 16 and terrain2.spec's row 6 both
    address cells that fall off the bottom of their sheets.
    """
    if sheet not in SHEETS:
        die("unknown sheet %r" % sheet)
    fname, x0, y0, dx, dy, border = SHEETS[sheet]
    x = x0 + (dx + border) * col
    y = y0 + (dy + border) * row
    w, h = SHEET_SIZE[sheet]
    if x + dx > w or y + dy > h:
        die("sprite %s(%d,%d) -> %d,%d falls outside %s (%dx%d)"
            % (sheet, row, col, x, y, fname, w, h))
    return "img/%s -%dpx -%dpx" % (fname, x, y)


# Terrain name -> the amplio section it binds to, and the sprite layers that
# section declares in space.tilespec. Layers are drawn in order.
TERRAIN_ART = {
    "Star":               ("star",              [("terrain1", 10, 0)]),
    "Inner System":       ("grassland",         [("terrain1",  2, 0)]),
    "Middle System":      ("plains",            [("terrain1",  1, 0)]),
    "Outer System":       ("arctic",            [("terrain1",  7, 0)]),
    "Asteroid Belt":      ("hills",             [("terrain1",  4, 0), ("hills",    0, 0)]),
    "Kuiper Belt":        ("forest",            [("terrain1",  3, 0), ("terrain2", 4, 0)]),
    "Inaccessible":       ("inaccessible",      [("terrain1", 10, 0)]),
    "Near Space":         ("coast",             [("ocean",     4, 4)]),
    "Interstellar Space": ("floor",             [("ocean",     8, 8)]),
}

BODY_ART = {
    "Molten Planet": ("ts.furs",                ("terrain1",  5, 4)),
    "Molten Moon":   ("ts.peat",                ("terrain1",  7, 2)),
    "Toxic Planet":  ("ts.arctic_ivory",        ("terrain1",  6, 2)),
    "Toxic Moon":    ("ts.silk",                ("terrain1",  2, 4)),
    "Rocky Planet":  ("ts.wine",                ("terrain1",  3, 4)),
    "Rocky Moon":    ("ts.spice",               ("terrain1",  7, 4)),
    "Gas Giant":     ("ts.whales",              ("terrain1",  9, 4)),
    "Ice Planet":    ("ts.buffalo",             ("terrain1",  1, 2)),
    "Ice Moon":      ("ts.grassland_resources", ("terrain1", 11, 4)),
}

OVERLAY = {
    "mine":       ("tx.mine",       ("terrain1", 3, 6)),
    "irrigation": ("tx.irrigation", ("terrain1", 1, 6)),
}

# Radially outward from the star, which is the order a player meets them.
RADIAL = ["Star", "Inner System", "Middle System", "Asteroid Belt",
          "Outer System", "Kuiper Belt", "Near Space", "Interstellar Space"]


# --------------------------------------------------------------------------
# Reading the ruleset
# --------------------------------------------------------------------------

def load():
    terr = parse(RULESET / "terrain.ruleset")
    effects = parse(RULESET / "effects.ruleset")

    terrains, bodies = {}, {}

    for name, sec in terr.items():
        if name.startswith("terrain_"):
            label = unquote(sec.get("name", ""))
            if label in TERRAIN_ART:
                terrains[label] = sec
        elif name.startswith("resource_"):
            # A [resource_*] section names its extra rather than itself.
            label = unquote(sec.get("extra", ""))
            if label in BODY_ART:
                bodies[label] = {"res": sec}

    missing = set(TERRAIN_ART) - set(terrains)
    if missing:
        die("terrain.ruleset has no section for: %s" % ", ".join(sorted(missing)))
    missing = set(BODY_ART) - set(bodies)
    if missing:
        die("terrain.ruleset has no resource for: %s" % ", ".join(sorted(missing)))

    # Each body's extra carries the flag its effects key off. Gas Giant has no
    # moon and so no flag; its effects name the extra directly.
    for name, sec in terr.items():
        if not name.startswith("extra_"):
            continue
        label = unquote(sec.get("name", ""))
        if label in bodies:
            flags = strlist(sec.get("flags", ""))
            bodies[label]["flag"] = flags[0] if flags else None

    for label, body in bodies.items():
        if "flag" not in body:
            die("no [extra_*] section found for body %r" % label)

    # Output_Add_Tile effects, indexed by (key, output type) where key is the
    # ExtraFlag name for a flagged body or the Extra name for Gas Giant.
    add_tile = {}
    for name, sec in effects.items():
        if not name.startswith("effect_") or sec.get("type", "") != '"Output_Add_Tile"':
            continue
        rows = sec.get("__table__")
        if not rows:
            continue
        # A city-centre variant grants irrigation food without the Irrigation
        # extra; it duplicates the plain irrigation value, so skip it here.
        if "center" in name:
            continue
        key = otype = improvement = None
        for row in rows[1:]:
            cells = strlist(row)
            if len(cells) < 3:
                continue
            rtype, rname = cells[0], cells[1]
            if rtype == "ExtraFlag":
                key = rname
            elif rtype == "Extra" and rname in ("Mine", "Irrigation"):
                improvement = rname
            elif rtype == "Extra":
                key = rname
            elif rtype == "OutputType":
                otype = rname
        if key and otype and improvement:
            add_tile[(key, improvement, otype)] = num(sec, "value")

    return terrains, bodies, add_tile


def yields(terrain, res):
    """Base output of a tile: terrain plus whatever resource sits on it."""
    return [num(terrain, k, 0) + num(res, k, 0) for k in ("food", "shield", "trade")]


def compose(terrain, body, add_tile):
    """The three states a worked body tile can be in.

    Mirrors city_tile_output(): terrain and resource output, then the
    terrain's own mining/irrigation increment, then the Output_Add_Tile
    effects that carry the body's yield.
    """
    key = body["flag"] or body["label"]
    base = yields(terrain, body["res"])

    mined = list(base)
    mined[1] += num(terrain, "mining_shield_incr", 0)
    mined[1] += add_tile.get((key, "Mine", "Shield"), 0)

    irrigated = list(base)
    irrigated[0] += num(terrain, "irrigation_food_incr", 0)
    irrigated[0] += add_tile.get((key, "Irrigation", "Food"), 0)

    return base, mined, irrigated


# --------------------------------------------------------------------------
# HTML
# --------------------------------------------------------------------------

CSS = """
:root {
  --bg:#0b0e17; --panel:#141926; --line:#242b3d; --ink:#c9d1e0;
  --dim:#6b7690; --head:#e8edf7;
  --food:#7dc98a; --shield:#d9a441; --trade:#6fb3e0;
}
* { box-sizing:border-box }
body { margin:0; padding:2.5rem 1.5rem 4rem; background:var(--bg); color:var(--ink);
       font:15px/1.6 system-ui,-apple-system,Segoe UI,Roboto,sans-serif }
main { max-width:60rem; margin:0 auto }
h1 { font-size:1.7rem; color:var(--head); margin:0 0 .3rem; font-weight:600 }
h2 { font-size:1.15rem; color:var(--head); margin:3rem 0 .4rem; font-weight:600 }
h2:first-of-type { margin-top:2rem }
p { margin:.5rem 0 }
.lede { color:var(--dim); margin-bottom:0 }
.note { color:var(--dim); font-size:.9rem }

/* A sprite is a 96x48 window onto a sheet; layers stack in draw order. */
.tile { position:relative; width:96px; height:48px; flex:none }
.tile i { position:absolute; inset:0; background-repeat:no-repeat;
          image-rendering:pixelated }

.strip { display:flex; flex-wrap:wrap; gap:.75rem 1.25rem; margin:1rem 0 0;
         padding:0; list-style:none }
.strip li { text-align:center; width:96px }
.strip span { display:block; font-size:.78rem; color:var(--dim); margin-top:.15rem }

table { border-collapse:collapse; width:100%; margin-top:.75rem; font-size:.88rem }
th,td { text-align:left; padding:.45rem .6rem; border-bottom:1px solid var(--line);
        vertical-align:top }
th { color:var(--dim); font-weight:500; font-size:.8rem; text-transform:uppercase;
     letter-spacing:.04em; white-space:nowrap }
td.n { text-align:right; font-variant-numeric:tabular-nums; white-space:nowrap }
tbody tr:hover { background:var(--panel) }
.help { color:var(--dim); font-size:.85rem }

.body { border-top:1px solid var(--line); padding:1.1rem 0 }
.body h3 { margin:0 0 .1rem; font-size:1rem; color:var(--head); font-weight:600 }
.body .where { color:var(--dim); font-size:.85rem; margin:0 0 .7rem }
.states { display:flex; flex-wrap:wrap; gap:1.5rem }
.state { text-align:center }
.state b { display:block; font-weight:500; font-size:.8rem; color:var(--dim);
           margin-bottom:.3rem; text-transform:uppercase; letter-spacing:.04em }
.out { margin-top:.35rem; font-size:.85rem; font-variant-numeric:tabular-nums }
.out span { margin:0 .18rem }
.f { color:var(--food) } .s { color:var(--shield) } .t { color:var(--trade) }
.z { color:var(--dim) }
.up { font-weight:700 }

footer { margin-top:3.5rem; padding-top:1.2rem; border-top:1px solid var(--line);
         color:var(--dim); font-size:.82rem }
footer a { color:var(--dim) }
"""


def esc(s):
    return html.escape(s, quote=False)


def tile_html(layers, extra=None):
    """A stack of sprite layers, drawn in order, optionally with an overlay."""
    parts = []
    for sheet, row, col in layers:
        url, x, y = sprite(sheet, row, col).split(" ")
        parts.append('<i style="background:url(%s) %s %s"></i>' % (url, x, y))
    if extra:
        sheet, row, col = extra
        url, x, y = sprite(sheet, row, col).split(" ")
        parts.append('<i style="background:url(%s) %s %s"></i>' % (url, x, y))
    return '<div class="tile">%s</div>' % "".join(parts)


def out_html(vals, base=None):
    """food / shield / trade, dimming zeros and bolding anything gained."""
    cells = []
    for val, cls in zip(vals, ("f", "s", "t")):
        klass = cls if val else "z"
        if base is not None and val > base[("f", "s", "t").index(cls)]:
            klass += " up"
        cells.append('<span class="%s">%d</span>' % (klass, val))
    return '<div class="out">%s</div>' % "".join(cells)


def render(terrains, bodies, add_tile):
    o = []
    w = o.append

    w("<!doctype html>")
    w('<html lang="en"><head><meta charset="utf-8">')
    w('<meta name="viewport" content="width=device-width,initial-scale=1">')
    w("<title>Space ruleset - terrain</title>")
    w("<style>%s</style></head><body><main>" % CSS)

    w("<h1>Space terrain</h1>")
    w('<p class="lede">Every terrain and celestial body in the space ruleset, '
      "drawn as the space tileset draws it, with what a city gets for working "
      "the tile. Generated from the ruleset &mdash; see the note at the foot.</p>")

    # 1. The system, from the star outward.
    w("<h2>A system, from the star outward</h2>")
    w("<p>A star system is a disc of concentric rings. The three rings feed a "
      "colony, the two belts supply its ore, and beyond them is the void "
      "between systems.</p>")
    w('<ul class="strip">')
    for name in RADIAL:
        _, layers = TERRAIN_ART[name]
        w("<li>%s<span>%s</span></li>" % (tile_html(layers), esc(name)))
    w("</ul>")

    # 2. Terrain table.
    w("<h2>What each terrain is worth</h2>")
    w("<p>Base output is what an unimproved tile produces. The irrigation and "
      "mining columns are what building the improvement <em>adds</em>, with the "
      "worker turns it costs. Nothing here produces trade except Near Space and "
      "a road.</p>")
    w("<table><thead><tr>")
    for head in ("", "Terrain", "Food", "Shield", "Trade", "Irrigate",
                 "Mine", "Road", "Move", "Def."):
        w("<th>%s</th>" % head)
    w("</tr></thead><tbody>")

    for name in RADIAL + ["Inaccessible"]:
        sec = terrains[name]
        _, layers = TERRAIN_ART[name]
        f, s, t = (num(sec, k, 0) for k in ("food", "shield", "trade"))

        def improvement(incr_key, time_key, unit):
            incr = num(sec, incr_key, 0)
            time = num(sec, time_key, 0)
            if not time:
                return '<span class="z">&mdash;</span>'
            turns = '<span class="z">&middot; %d turn%s</span>' % (time, "" if time == 1 else "s")
            if not incr:
                return '<span class="z">+0</span> %s' % turns
            return '+%d%s %s' % (incr, unit, turns)

        road = num(sec, "road_time", 0)
        road_txt = ('+1<span class="t">t</span> <span class="z">&middot; %d turns</span>' % road
                    if road else '<span class="z">&mdash;</span>')
        defense = num(sec, "defense_bonus", 0)

        w("<tr>")
        w("<td>%s</td>" % tile_html(layers))
        w("<td><strong>%s</strong><br><span class=\"help\">%s</span></td>"
          % (esc(name), esc(unquote(sec.get("helptext", "")).replace("\n\n", " "))))
        for val, cls in ((f, "f"), (s, "s"), (t, "t")):
            w('<td class="n"><span class="%s">%d</span></td>'
              % (cls if val else "z", val))
        w('<td class="n">%s</td>' % improvement("irrigation_food_incr",
                                                "irrigation_time", '<span class="f">f</span>'))
        w('<td class="n">%s</td>' % improvement("mining_shield_incr",
                                                "mining_time", '<span class="s">s</span>'))
        w('<td class="n">%s</td>' % road_txt)
        w('<td class="n">%s</td>' % (num(sec, "movement_cost", 0) or
                                     '<span class="z">&mdash;</span>'))
        w('<td class="n">%s</td>' % ("%d%%" % defense if defense
                                     else '<span class="z">&mdash;</span>'))
        w("</tr>")
    w("</tbody></table>")
    w('<p class="note">The three rings carry a mining time but no mining bonus. '
      "That is deliberate: freeciv refuses Build Mine outright on a terrain with "
      "no mining time, so without it a world sitting on a ring could never be "
      "mined. A mine on a bare ring tile yields nothing.</p>")

    # 3. Bodies.
    w("<h2>Worlds</h2>")
    w("<p>A planet or moon is not a terrain but a resource sitting on a ring, "
      "which is what lets one ring hold several kinds of world. Every world is "
      "worth 2 trade unworked; a gas giant also gives 2 shields. What separates "
      "them is what you can make of them &mdash; and a planet is worth exactly "
      "what its moon is.</p>")

    for label in BODY_ART:
        body = bodies[label]
        body["label"] = label
        rings = [t for t in RADIAL
                 if label in strlist(terrains[t].get("resources", ""))]
        if not rings:
            die("no terrain lists %r in its resources line" % label)

        w('<div class="body">')
        w("<h3>%s</h3>" % esc(label))
        w('<p class="where">on %s</p>' % esc(" and ".join(rings)))
        w('<div class="states">')
        for ring in rings:
            _, layers = TERRAIN_ART[ring]
            _, art = BODY_ART[label]
            base, mined, irrigated = compose(terrains[ring], body, add_tile)
            stack = layers + [art]
            for title, vals, over in (
                    ("bare", base, None),
                    ("mined", mined, OVERLAY["mine"][1]),
                    ("irrigated", irrigated, OVERLAY["irrigation"][1])):
                w('<div class="state"><b>%s%s</b>%s%s</div>'
                  % (title,
                     "" if len(rings) == 1 else " &middot; %s" % ring.split()[0].lower(),
                     tile_html(stack, over),
                     out_html(vals, base if over else None)))
        w("</div></div>")

    w('<p class="note" style="margin-top:1.2rem">Numbers are food, shield, '
      "trade. A tile can be mined or irrigated, never both &mdash; the two "
      "conflict. A road adds 1 trade to any of the five workable terrains and "
      "appears free on a city centre.</p>")

    w("<footer>")
    w("<p>Generated by <code>docs/space/gen_terrain.py</code> from "
      "<code>data/space/terrain.ruleset</code> and "
      "<code>data/space/effects.ruleset</code>. Re-run it after any balance "
      "change rather than editing this page.</p>")
    w("<p>Art from the space tileset, a repainted amplio2: space tiles, crystal "
      "and space station by AlexusBlack; main star and planets by Viktor; space "
      "organics by Wenrexa; hills by Peter Arbor; forest from Battle for "
      "Wesnoth. GPL v2.</p>")
    w("</footer>")
    w("</main></body></html>")
    return "\n".join(o) + "\n"


def main():
    for key, (fname, *_rest) in SHEETS.items():
        path = IMG / fname
        if not path.exists():
            die("missing sprite sheet %s (copy it from "
                "tilesets/space/space_tiles/)" % path)
        SHEET_SIZE[key] = png_size(path)

    terrains, bodies, add_tile = load()

    expected = 10  # 5 bodies x mine + irrigation
    if len(add_tile) != expected:
        die("found %d Output_Add_Tile body effects, expected %d"
            % (len(add_tile), expected))

    OUT.write_text(render(terrains, bodies, add_tile), encoding="utf-8")
    print("wrote %s (%d terrains, %d worlds, %d effects)"
          % (OUT.relative_to(ROOT), len(terrains), len(bodies), len(add_tile)))


if __name__ == "__main__":
    main()

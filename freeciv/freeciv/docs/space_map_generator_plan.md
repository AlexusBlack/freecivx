# Space Map Generator (`MAPGEN_SPACE`) — Design & Implementation Plan

## Overview

A new map generator that renders the map as a region of interstellar space
containing a scattering of **star systems**. Each system is a roughly circular
"island" of playable terrain; the space between systems is ocean-class
"interstellar space" containing nothing.

At the centre of the map sits **Sol**, a hand-authored system of fixed
composition (its orbital angles are randomised per map) that is the
common starting point for *all* players. Every other system is generated
procedurally and is an expansion target.

Intended to be used together with a dedicated **space ruleset** and **space
tileset** (neither exists in the tree yet — `data/alien/` is the closest
precedent for a non-Earth ruleset).

### Target structure of a procedural system

| Ring | Radius band | Terrain analogue | Contents |
|------|-------------|------------------|----------|
| Core | centre | `Star` (own art, §13) | 1–3 stars |
| Inner | inner band | Grassland (most irrigable) | 0–3 molten / toxic planets, **no moons** |
| Middle | middle band | Plains (less irrigable) | 0–3 rocky planets, each with 1–3 rocky moons |
| Outer | outer band | **Arctic** (least irrigable) | 0–2 gas giants (3–8 moons each), 0–2 ice planets (0–2 moons each) |
| Belts | 1–3 annuli, **any ring** | Hills | asteroid belts |
| Kuiper | outermost annulus, optional | **Forest** | Kuiper belt |
| — | outside | Ocean class | interstellar space, **no resources** |

> The terrain analogue column names the **tileset art** each ring borrows, not a
> ruleset terrain — the ruleset terrains are in §3.1 and the art binding is §13.
> Outer was Tundra and Kuiper was Mountains in the first draft; both were
> changed once the actual art was inspected (§13.1).

Systems are separated by **5–15 interstellar tiles**.

### Settled design decisions

| Question | Decision |
|---|---|
| Planets & moons | **Resources** (extras with cause `EC_RESOURCE`), placed by our own pass |
| Moon output | Moons produce output **by themselves**, via their own `output[]` — no parent-relative effects |
| Huts | **None.** Generator sets `have_huts = TRUE` to suppress `make_huts()` |
| Asteroid belts | May appear in **any** ring (inner, middle, or outer) |
| Start positions | All players start in **Sol**; the generator creates the startpos objects itself |
| Earth-like terrains in the ruleset | **Yes** — kept so other generators remain usable (see §3.1) |
| Topology | **Hex is rejected.** Non-hex (square / iso-square) only |

---

## 1. How this fits the existing generator pipeline

Entry point is `map_fractal_generate()` — `server/generator/mapgen.c:1274`.
It runs, in order:

```
seed RNG                                mapgen.c:1283-1300
generator_init_topology(autosize)       mapgen_topology.c:164
main_map_allocate()
adjust_terrain_param()                  mapgen.c:1495   (Earth-centric; irrelevant to us)
create_tmap(FALSE)                      temperature_map.c:118  (dummy tmap = colatitude)
── generator-specific branch ──         mapgen.c:1321-1374
remove_tiny_islands()                   mapgen.c:1189   (unless 'tinyisles')
smooth_water_depth()                    mapgen_utils.c:612
assign_continent_numbers()              mapgen_utils.c
regenerate_lakes()                      mapgen_utils.c:350
add_resources(map.server.riches)        mapgen.c:1579
make_huts(huts * num_tiles / 1000)      mapgen.c:1538
create_start_positions(mode, utype)     startpos.c:300
destroy_tmap(); print_mapgen_map()
```

The existing generators split into two families:

* **Height-map family** (`RANDOM`, `FRACTAL`, `FRACTURE`) — produce only
  `height_map[]`, then share `make_land()` (`mapgen.c:1060`).
* **Island family** (`ISLAND`, `FAIR`) — never build a height map; they stamp
  pre-shaped islands tile-by-tile (`mapgenerator2/3/4`, `mapgen.c:2238-2497`;
  `map_generate_fair_islands`).

**`MAPGEN_SPACE` belongs structurally to the island family**, and specifically
follows the `MAPGEN_FAIR` pattern: it produces terrain, resources *and* start
positions itself, then flags the shared passes off. We never call `make_land()`,
`make_relief()`, `make_terrains()`, or `make_rivers()`.

### Which shared stages we keep

| Stage | Keep? | Why |
|-------|-------|-----|
| `generator_init_topology()` | yes | needed for xsize/ysize and `map_init_topology()` |
| `create_tmap(FALSE)` | leave alone | nothing we do consumes it, but `destroy_tmap()` at `mapgen.c:1484` asserts it is non-NULL — see §7.3 |
| `remove_tiny_islands()` | harmless | we never emit 1-tile islands; see §7.4 |
| `smooth_water_depth()` | **yes, exploit it** | grades ocean by `property_ocean_depth` vs. distance to land (`mapgen_utils.c:612`) — a free "near space → deep space" halo around systems |
| `assign_continent_numbers()` | yes | one continent per system |
| `regenerate_lakes()` | no-op | returns immediately if the ruleset has no `TER_FRESHWATER` terrain (`mapgen_utils.c:380`) |
| `add_resources()` | **suppress** | its 1-tile minimum spacing (`is_resource_close()`, `mapgen.c:1565`) makes moon-adjacent-to-planet impossible. Set `have_resources = TRUE` |
| `make_huts()` | **suppress** | no huts wanted. Set `have_huts = TRUE` |
| `create_start_positions()` | **suppress** | skipped automatically when `map_startpos_count() != 0` (`mapgen.c:1412`); we place all players in Sol ourselves |

---

## 2. Planets & moons as resources

Resources are the right vehicle:

* One per tile (`ptile->resource`) — matches "one celestial body per tile".
* Output comes straight from `presource->data.resource->output[]`
  (`common/city.c:1285-1286`), so **moons produce output by themselves** (OQ4)
  with no effects needed.
* Tilesets already have a resource drawing layer.
* **Zone rules become partly ruleset-enforced for free**: `tile_set_resource()`
  only actually attaches the extra when
  `terrain_has_resource(ptile->terrain, presource)` (`common/tile.c:344-360`).
  So listing `Molten Planet` / `Toxic Planet` only under the inner-ring terrain,
  `Gas Giant` only under the outer-ring terrain, etc., means a placement bug
  cannot produce a gas giant in the inner ring.
* Cost: a moon cannot share a tile with its planet — moons occupy neighbouring
  tiles. That is exactly the requested behaviour.

Because `add_resources()` is suppressed, the generator owns every resource on
the map and there is no risk of stray resources appearing in interstellar space.
(Belt and suspenders: the interstellar terrains also carry an empty
`resources =` list.)

> ⚠️ **Gotcha:** `tile_set_resource()` sets `ptile->resource = presource`
> *unconditionally* but only calls `tile_add_extra()` when
> `terrain_has_resource()` passes. A mismatch therefore half-applies: the tile
> claims a resource, `tile_resource_is_valid()` (`common/tile.c:150`) returns
> FALSE, and the tile produces nothing. Always guard with an explicit
> `terrain_has_resource()` check plus `fc_assert` before calling.

---

## 3. Ruleset requirements (space ruleset)

### 3.1 Terrains

| Rule name (proposed) | Class | Key properties / flags | Notes |
|---|---|---|---|
| `Star` | Land | `TER_NOT_GENERATED`, **not** `TER_STARTER` | unworkable or high-shield centre |
| `Inner System` | Land | high food, `TER_STARTER`, `TER_NOT_GENERATED` | grassland analogue |
| `Middle System` | Land | medium output, `TER_STARTER`, `TER_NOT_GENERATED` | plains analogue |
| `Outer System` | Land | low output, `TER_STARTER`, `TER_NOT_GENERATED` | arctic art |
| `Asteroid Belt` | Land | mine-able, low food, `TER_NOT_GENERATED` | hills art |
| `Kuiper Belt` | Land | mine-able, `TER_NOT_GENERATED` | forest art |
| `Near Space` | Oceanic | `property_ocean_depth = 0`, `TER_NOT_GENERATED` | halo around systems |
| `Interstellar Space` | Oceanic | `property_ocean_depth = 80`, `TER_NOT_GENERATED`, empty `resources` | deep space |
| *(Earth-like set)* | mixed | normal `property_*`, **no** `TER_NOT_GENERATED` | see below |

**On `TER_NOT_GENERATED` (OQ3 = yes).** The ruleset keeps a conventional
Earth-like terrain set so `generator=RANDOM`/`FRACTAL`/`ISLAND`/`FRACTURE`
remain usable with it. The mechanism that keeps the two sets from contaminating
each other is the `TER_NOT_GENERATED` flag:

* `pick_terrain()`, `pick_terrain_by_flag()`, `pick_ocean()`,
  `regenerate_lakes()` and `make_land()`'s `land_fill` selection **all skip**
  `TER_NOT_GENERATED` terrains (`mapgen_utils.c:713, 733, 750`;
  `mapgen_utils.c:361`; `mapgen.c:1072`). So the stock generators will never
  place a space terrain.
* `MAPGEN_SPACE` selects terrain by **rule name**, not by property, and does not
  consult the flag. So it places space terrains freely and will never place an
  Earth terrain.

This gives a clean two-mode ruleset with no extra machinery. The `property_*`
values on space terrains are then irrelevant and should be left at 0.

Other requirements derived from code:

* At least one **generated** land terrain must exist or `make_land()` hits its
  `fc_assert_exit_msg` (`mapgen.c:1078`) when someone runs a stock generator —
  the Earth-like set satisfies this.
* `TER_STARTER` on the ring terrains is not strictly needed by `MAPGEN_SPACE`
  (we bypass `create_start_positions()`), but keep it for the map editor and for
  scenario reuse.
* No space terrain should carry `TER_FRESHWATER`, so `regenerate_lakes()` stays
  inert on space maps.
* `property_ocean_depth` on the two space terrains drives `smooth_water_depth()`;
  `OCEAN_DIST_MAX` is `TERRAIN_OCEAN_DEPTH_MAXIMUM / 25` = 4, so with 5–15 tile
  gaps the halo lands nicely between systems.

### 3.2 Resources (planets & moons)

| Resource | Listed under terrain | Placed by |
|---|---|---|
| `Molten Planet`, `Toxic Planet` | `Inner System` | inner pass |
| `Rocky Planet` | `Middle System` | middle pass |
| `Rocky Moon` | `Middle System`, `Outer System` | moon pass |
| `Gas Giant` | `Outer System` | outer pass |
| `Ice Planet` | `Outer System` | outer pass |
| `Molten Moon`, `Toxic Moon`, `Ice Moon` | `Outer System` | moon pass |

Each carries its own `output[]` (food / shield / trade), since moons are
self-sufficient producers (OQ4).

`resource_freq` is only read by `pick_resource()` (`mapgen_utils.c:801`), which
we bypass — but set sane values anyway so the ruleset stays usable with the
stock generators and the map editor.

### 3.3 Huts

None. No `cause = "Hut"` extras are required by this generator, and
`make_huts()` is suppressed. If the ruleset defines huts for other generators'
benefit, `MAPGEN_SPACE` simply never places them.

---

## 4. Generator algorithm

New files: `server/generator/space_map.c` / `space_map.h`, added to
`server/generator/Makefile.am`.

Public entry point:

```c
bool map_generate_space(void);   /* FALSE => generation failed */
```

### Phase 0 — Validation, sizing, feasibility

1. **Topology check** (OQ6):

   ```c
   if (current_topo_has_flag(TF_HEX)) {
     log_error(_("The space map generator does not support hex topologies."));
     return FALSE;
   }
   ```

   On non-hex maps `map_vector_to_sq_distance()` is `dx*dx + dy*dy`
   (`common/map.c:622-623`), so `circle_dxyr_iterate` yields true circles.
   Wrapping in X and/or Y is fine.

2. **Ruleset check** — look up every required terrain and resource by rule name
   (`terrain_by_rule_name()`, `extra_type_by_rule_name()`). Any miss → log a
   clear error naming the missing entry and return FALSE.

3. **Sol radius**, which must host every player:

   ```
   R_sol = MAX(SOL_MIN_RADIUS,                     /* 10 — the template's own minimum */
               ceil(sqrt(player_count() * TILES_PER_START / M_PI)))
   ```

   with `TILES_PER_START ≈ 22`. (2–16 players → R_sol = 10–11; 30 players → 15.)

4. **Procedural system radius** `R` and **count** `N` from map size and
   `landpercent`:

   ```
   R  ∈ [4, 9], drawn per system
   N  chosen so that   π·R_sol² + Σ π·R_i²  ≈  map_num_tiles() · landpercent/100
   ```

   Aim for `N >= player_count()` expansion targets. Note the
   `num_continents >= player_count() + 3` rule in `startpos.c:392` **does not
   apply to us** — we bypass `create_start_positions()` entirely.

5. **Area feasibility**: `π(R_sol + gap)² + Σ π(R_i + gap/2)² <= map_num_tiles() · PACKING_EFFICIENCY`
   (use ~0.75). If it fails, shrink `R` (floor 4), then `N`. If Sol alone does
   not fit, return FALSE.

### Phase 1 — System centre placement

1. **Sol first**, pinned at the map centre:

   ```c
   sol->centre = native_pos_to_tile(&wld.map, wld.map.xsize / 2, wld.map.ysize / 2);
   sol->radius = R_sol;
   ```

2. **Procedural systems** by dart throwing (Poisson-disk):

   ```
   for attempt in 1..MAX_ATTEMPTS:
       t = rand_map_pos(&wld.map)
       reject if for any placed centre c:
           real_map_distance(t, c) < R(t) + R(c) + MIN_GAP      /* MIN_GAP = 5 */
       accept
   ```

   `real_map_distance()` (`common/map.c:630`) is used rather than raw
   coordinates so map wrapping is respected.

3. **Relaxation** — for any system whose nearest neighbour is further than
   `R_a + R_b + MAX_GAP` (15), nudge it toward that neighbour. This is what
   actually delivers "5–15 tiles between systems" rather than merely "at least
   5". Sol is pinned and never moves.

Store the systems in a dynamically grown array of `struct space_system` (§5).

### Phase 2 — Fill background

Set every tile to `Interstellar Space`. `smooth_water_depth()` later converts
the near-system band to `Near Space`.

### Phase 3 — Carve rings

Radii, given a system's outer radius `R`:

```
R_inner  = round(0.30 * R)
R_middle = round(0.60 * R)
R_outer  = round(0.90 * R)
R_kuiper = R                      /* the outermost annulus */
```

Iterate with `circle_dxyr_iterate(nmap, centre, R*R, ptile, dx, dy, dr)`
(`common/map.h:406`); `dr` is the **squared** distance.

To avoid mechanically perfect discs, perturb the ring boundary by angle:

```
theta   = atan2(dy, dx)
wobble  = Σ over k in {2,3,5} of  A_k · sin(k·theta + phase_k)    /* per-system */
r_eff   = sqrt(dr) - wobble                                        /* A_k ≈ 0.6 tiles */
```

Assign terrain from `r_eff`: `<= R_inner` → `Inner System`,
`<= R_middle` → `Middle System`, `<= R_outer` → `Outer System`,
`<= R_kuiper` → `Outer System` (the Kuiper annulus is overwritten below if
present).

**Stars:** `n_stars = 1 + fc_rand(3)`. Place `Star` terrain on the centre tile
and, for `n_stars > 1`, on additional tiles with `dr <= 2`.

**Asteroid belts (any ring):** choose `n_belts = 1 + fc_rand(3)`. Each belt is a
width-1 annulus at a radius drawn uniformly from `[2, R_outer]` — i.e. it may
land in the inner, middle *or* outer ring — subject to being at least 2 tiles
from any previously chosen belt radius. Angular coverage 60–100 %: walk the
annulus and skip runs of tiles to leave gaps. Overwrite the ring terrain with
`Asteroid Belt`.

**Kuiper belt:** with probability ~50 %, an annulus at
`r_eff ∈ (R_outer, R_kuiper]`, width 1–2, 50–90 % coverage, terrain
`Kuiper Belt`.

Mark every written tile in the shared `placed_map`
(`create_placed_map()` / `map_set_placed()`, `mapgen_utils.h`).

### Phase 3b — Sol overrides Phase 3

Sol is **not** procedurally generated. It uses the ring radii above (with
`R = R_sol`) and the wobble. Its **composition is fixed** — the same bodies at
the same orbital radii on every map — but each body's **orbital angle is drawn
per map**, so Sol's layout varies between games while its content does not.

| # | Orbit (fraction of `R_sol`) | Angle | Body | Moons | Clearance |
|---|---|---|---|---|---|
| 0 | centre | — | `Star` ×1 | — | — |
| 1 | 0.20 | random | `Molten Planet` | 0 | 3 |
| 2 | 0.28 | random | `Toxic Planet` | 0 | 3 |
| 3 | 0.42 | random | `Rocky Planet` | 1 × `Rocky Moon` | 4 |
| 4 | 0.52 | random | `Rocky Planet` | 2 × `Rocky Moon` | 4 |
| 5 | 0.62 | — | **Asteroid Belt** (annulus, 85 % coverage) | — | — |
| 6 | 0.72 | random | `Gas Giant` | 5 (mixed) | 5 |
| 7 | 0.80 | random | `Gas Giant` | 3 (mixed) | 5 |
| 8 | 0.86 | random | `Gas Giant` | 1 (mixed) | 5 |
| 9 | 0.90 | random | `Ice Planet` | 0 | 3 |
| 10 | 1.00 | — | **Kuiper Belt** (annulus, 80 % coverage) | — | — |

#### Angle assignment

Angles are drawn by rejection sampling against a **minimum separation**, so
neighbouring bodies — in particular the three gas giants, whose orbits are close
together — never contend for the same moon tiles.

Because two bodies on *different* orbits are further apart than their angular
difference alone suggests, the constraint is expressed as a required chord
distance and checked exactly:

```
d(a, b) = sqrt( r_a² + r_b² − 2·r_a·r_b·cos(θ_a − θ_b) )
required = MAX(clearance_a, clearance_b)
accept iff  d(a, b) >= required   for every already-placed body b
```

`clearance` is the radius of tiles a body needs to itself: 5 for a gas giant
(its moons fill the 8-neighbourhood and may spill to `sq_map_distance <= 4`),
4 for a rocky planet, 3 for a moonless planet.

For the rejection sampler, the equivalent angular floor on a shared orbit is
useful as a sanity bound:

```
Δθ_min ≈ 2·asin( required / (2·min(r_a, r_b)) )
```

For the three gas giants at `R_sol = 10` (orbits 7.2, 8.0, 8.6) with
`required = 5`, that is ≈ 0.72 rad ≈ **41°** — so three gas giants need ≈ 123°
of the available 360°, leaving ample slack.

Draw order is **outermost first** (gas giants, then ice planet, then rocky, then
inner), because the outer bodies have the largest clearance and the fewest
degrees of freedom relative to their orbit length. If the sampler fails after
`MAX_ANGLE_TRIES` (~200) for any body, fall back to deterministic even spacing
within that body's class plus a ±10° jitter, and log at verbose level.

Sol's body counts are **fixed** and are *not* scaled by `riches` (§6.1); only
the angles vary.

### Phase 4 — Celestial bodies (procedural systems)

Maintain a local occupancy array (`bool *body_taken`, `MAP_INDEX_SIZE`) — the
`placed_map` is about terrain, not resources.

```c
static bool try_place_body(struct tile *ptile, struct extra_type *res)
{
  if (body_taken[tile_index(ptile)]) return FALSE;
  if (!terrain_has_resource(tile_terrain(ptile), res)) return FALSE;  /* §2 gotcha */
  tile_set_resource(ptile, res);
  body_taken[tile_index(ptile)] = TRUE;
  return TRUE;
}
```

Per system, in order (all counts then scaled by `riches`, §6.1):

1. **Inner** — `0–3` planets from `{Molten, Toxic}`. Candidate tiles: inner-ring
   tiles that are not `Star` or belt. Require `real_map_distance >= 3` from any
   planet already placed in this system. No moons.
2. **Middle** — `0–3` × `Rocky Planet`. For each, place `1–3` × `Rocky Moon` on
   free tiles with `sq_map_distance <= 2` (the 8 neighbours), preferring
   cardinal.
3. **Outer, gas giants** — `0–2` × `Gas Giant`, each with `3–8` moons of any
   type `{Molten, Toxic, Rocky, Ice}`. The 8-neighbourhood holds at most 8 moons
   and is often partly occupied, so search the ring of `sq_map_distance <= 4`
   (24 tiles) sorted by distance, and require a gas giant centre to be ≥ 4 tiles
   from any other planet so its neighbourhood is reservable.
4. **Outer, ice planets** — `0–2` × `Ice Planet`, each with `0–2` × `Ice Moon`,
   same adjacency rule as rocky planets.

Moons may spill into an adjacent ring but never onto belt tiles or outside
`R_kuiper` — enforced automatically by the `terrain_has_resource()` guard, given
the ruleset listings in §3.2.

If a body cannot be placed after `k` candidate tries, drop it silently rather
than failing the map.

### Phase 4b — Start positions in Sol

All players start in Sol. Following the `MAPGEN_FAIR` pattern
(`mapgen.c:3716-3726`):

```c
for (i = 0; i < player_count(); i++) {
  struct tile *ptile = sol_start_tile(i, player_count());
  struct startpos *psp = map_startpos_new(ptile);

  startpos_allows_all(psp);
}
```

`sol_start_tile()` spreads players by angle for symmetry and fairness:

```
theta  = 2π·i / player_count()
r      = R_inner + (R_middle - R_inner)/2          /* the good terrain band */
tile   = centre + (round(r·cos θ), round(r·sin θ))
```

then walks outward to the nearest tile that is: land, not `Star`, not a belt,
carries no resource (`!body_taken[...]`), and is ≥ 3 tiles from any previously
chosen start tile.

Because `map_startpos_count()` is then non-zero, `map_fractal_generate()` skips
`create_start_positions()` entirely (`mapgen.c:1412`).

> `startpos_allows_all(psp)` must be called — a fresh `startpos` allows no
> nation until told otherwise.

### Phase 5 — Finish

```c
wld.map.server.have_resources = TRUE;   /* suppress add_resources() */
wld.map.server.have_huts      = TRUE;   /* suppress make_huts()     */
destroy_placed_map();
free(body_taken);
return TRUE;
```

Continent numbering and water-depth smoothing are then handled by the shared
tail of `map_fractal_generate()`.

---

## 5. Data structures

```c
struct space_system {
  struct tile *centre;
  int radius;                  /* R_kuiper — the true outer edge */
  int r_inner, r_middle, r_outer;
  bool is_sol;
  int n_stars;
  int n_belts;
  int belt_r[3];               /* belt radii; any ring */
  bool has_kuiper;
  float wobble_amp[3];
  float wobble_phase[3];
  int continent;               /* filled after assign_continent_numbers() */
};
```

Module-local state:

```c
static struct space_system *systems;
static int num_systems;
static bool *body_taken;                    /* MAP_INDEX_SIZE */

/* Resolved once at generator start, by rule name. */
static struct terrain *ter_star, *ter_inner, *ter_middle, *ter_outer,
                      *ter_asteroid, *ter_kuiper, *ter_near, *ter_deep;
static struct extra_type *res_molten_planet, *res_toxic_planet,
                         *res_rocky_planet, *res_gas_giant, *res_ice_planet,
                         *res_rocky_moon, *res_molten_moon,
                         *res_toxic_moon, *res_ice_moon;
```

---

## 6. Server settings

Reuse existing settings where the meaning carries over.

| Setting | Reuse / new | Meaning here |
|---|---|---|
| `generator` | extend enum | new value `SPACE` |
| `landpercent` | reuse | target fraction of map inside systems — drives `N` and `R` |
| `riches` | reuse, redefined | celestial body density — see §6.1 |
| `steepness` | reuse, redefined | asteroid-belt count and Kuiper-belt probability |
| `mapsize` / `size` / `tilesperplayer` | reuse | unchanged |
| `huts` | **ignored** | no huts; log a notice if non-zero |
| `startpos` | **ignored** | all players start in Sol; log a notice if non-default |
| `temperature`, `wetness`, `separatepoles`, `flatpoles`, `tinyisles` | ignored | log a notice if non-default |
| `systemsize` (optional, new) | new | overrides computed procedural `R` |
| `systemgap` (optional, new) | new | overrides the 5–15 gap band |

Prefer overloading `riches`/`steepness` in v1; add dedicated settings only if
tuning proves awkward (they cost changes in `server/settings.c` plus the
settings enum).

### 6.1 The role of `riches` (OQ5)

**Stock meaning.** `riches` is 0–1000, default 250 (`common/map.h:664-666`). In
`add_resources()` it is a *per-tile probability out of 1000*
(`mapgen.c:1584`): each tile with no resource within 1 tile has a
`riches/1000` chance of getting one. That semantic is meaningless for us — we
suppress `add_resources()`, and our bodies are placed by structural rules, not
per-tile dice.

**New meaning: a body-density multiplier.**

```c
/* 250 (the stock default) maps to 1.0 */
static float richness(void)
{
  return (float) wld.map.server.riches / (float) MAP_DEFAULT_RICHES;
}
```

Applied as follows:

| Applies to | How |
|---|---|
| Planet count per ring (inner 0–3, middle 0–3, outer 0–2 + 0–2) | `n = scale(base_n)`, then clamped to the ring's spec maximum |
| Moon count per planet (1–3 / 3–8 / 0–2) | `n = scale(base_n)`, then clamped to the spec range |
| Asteroid / Kuiper belts | **not** affected — those are `steepness` |
| **Sol** | **not** affected — Sol's body list is fixed, so every game's shared start offers the same content regardless of settings (only its orbital angles vary) |

`scale()` uses stochastic rounding so that a fractional multiplier produces
*variety across systems* rather than uniformly rounding every system the same
way:

```c
static int scale(int base)
{
  float x = base * richness();
  int   n = (int) x;

  if (fc_rand(1000) < (int) ((x - n) * 1000)) {
    n++;
  }
  return n;
}
```

Behaviour across the range:

| `riches` | Multiplier | Result |
|---|---|---|
| 0 | 0.0 | Systems have terrain, stars and belts but **no planets or moons at all**. Sol is still fully populated, so the game is playable but expansion is worthless. A valid, deliberately harsh setting. |
| 125 | 0.5 | Sparse — roughly half the systems have any planet at all |
| 250 (default) | 1.0 | The spec ranges as written |
| 500 | 2.0 | Ranges doubled *before* clamping, so most systems sit at their spec maximum |
| 1000 | 4.0 | Every system is saturated; the clamps and the geometric capacity checks (free neighbour tiles for moons) become the real limit |

Note that above ~2.0 the multiplier saturates against two hard ceilings — the
per-ring spec maxima and the physical availability of free adjacent tiles for
moons — so `riches` has diminishing effect at the top of its range. That is
acceptable, but worth stating in the setting's help text so players are not
surprised that 1000 looks much like 600.

**Alternative considered and rejected:** scaling *system count* `N` with
`riches` instead. That conflates "how much stuff is out there" with "how far
apart it is", and `landpercent` already governs `N`.

---

## 7. Constraints and pitfalls found in the code

### 7.1 Start positions — we own them
`create_start_positions()` (`startpos.c:300`) is bypassed because we populate
`map.startpos_table` ourselves (`mapgen.c:1412` guards on
`map_startpos_count() == 0`). Consequences:

* The `num_continents >= player_count() + 3` demotion rule (`startpos.c:392`)
  and the `TER_STARTER` filter (`startpos.c:283`) never run on space maps.
  Sizing is therefore governed only by Sol's own capacity (Phase 0.3).
* A `case MAPGEN_SPACE:` must still be added to the mode-selection switch at
  `mapgen.c:1415` — the block is unreachable for us, but the switch is over the
  enum and will fail `-Wswitch` without it. Add it with a `break;` and a
  comment.
* `startpos_allows_all()` must be called on each new startpos.

### 7.2 The height map is a live landmine
`mapgen.c:1372-1373` has `free(height_map)` commented out in this fork, so
`height_map` outlives `make_land()`. Anything reusing the shared helpers that
call `hmap()` — `place_terrain()` (`mapgen.c:392`), `create_tmap(TRUE)`
(`temperature_map.c:139`), every `river_test_*`, `make_relief()`,
`ini_hmap_low_level()` / `map_pos_is_low()` — will read a **NULL or stale**
pointer if `MAPGEN_SPACE` never allocates it.

Mitigation: never touch those helpers. `mapgen_utils.c` and `startpos.c` are
entirely hmap-free (confirmed by grep), so everything we do reuse is safe.

### 7.3 The temperature map
Nothing in the space pipeline consumes it: `make_huts()` was the only consumer
via `rand_map_pos_characteristic()` → `condition_filter()` (`mapgen.c:246`), and
huts are now suppressed. But the framework creates it at `mapgen.c:1319` and
`destroy_tmap()` at `mapgen.c:1484` has `fc_assert_ret(NULL != temperature_map)`.

**Therefore: do not call `destroy_tmap()` and do not call `create_tmap()`.**
Leave the dummy map exactly as the framework set it up.

### 7.4 `remove_tiny_islands()`
`is_tiny_island()` (`mapgen.c:1163`) inspects `pterrain->property[MG_FROZEN]`,
which on our space terrains is 0. Systems are far larger than 1 tile so this is
a no-op — unless a heavily gapped Kuiper belt strands a single tile. Cheapest
insurance: skip this step for `MAPGEN_SPACE`, or keep belt coverage ≥ 50 % as
specified so gaps never isolate a tile.

### 7.5 Topology — hex rejected
`map_vector_to_sq_distance()` (`common/map.c:614`) uses `dx²+dy²` only on
**non-hex** topologies; on hex it squares the hex real-distance, so
`circle_dxyr_iterate` yields hexagons. Per OQ6 the generator **rejects hex
outright** (Phase 0.1) rather than supporting two geometries. Wrapping in X
and/or Y is supported, which is why Phase 1 uses `real_map_distance()`.

### 7.6 Resource validity
Covered in §2. Also note `tile_resource_is_valid()` (`common/tile.c:150`) is
re-checked on savegame load and on terrain change, so a terrain-changing unit
action could legitimately destroy a planet — decide in the ruleset whether ring
terrains are transformable at all.

---

## 8. Files to create / modify

**New**
* `server/generator/space_map.c` — the generator
* `server/generator/space_map.h` — `bool map_generate_space(void);`
* `data/space/` — ruleset (terrain, extras/resources, units, techs, …)
* `data/space.tilespec` + graphics — tileset

**Modified**
| File | Change |
|---|---|
| `common/map_types.h:52` | add `MAPGEN_SPACE` to `enum map_generator` |
| `server/settings.c:310` | add `NAME_CASE(MAPGEN_SPACE, "SPACE", N_("Star systems"))` |
| `server/settings.c:909` | review `generator_validate()` |
| `server/settings.c` (riches help) | document the redefined meaning under `SPACE` |
| `server/generator/mapgen.c:1321` | add the `MAPGEN_SPACE` branch, before the hmap branches |
| `server/generator/mapgen.c:1415` | add `case MAPGEN_SPACE:` (unreachable; `-Wswitch`) |
| `server/generator/Makefile.am` | add `space_map.c` / `space_map.h` |
| `data/Makefile.am` | install the new ruleset/tileset |

The branch in `map_fractal_generate()`:

```c
if (MAPGEN_SPACE == wld.map.server.generator
    && !map_generate_space()) {
  /* No sensible fallback: the other generators would produce an Earth map
   * on a space ruleset. Fail and let srv_main.c retry or abort. */
  destroy_tmap();
  return FALSE;
}
```

Falling back to `ISLAND` (as `FAIR` does at `mapgen.c:1323`) would silently
produce a broken game, so we fail instead and let `srv_main.c:3269` retry with a
different mapseed or abort with a clear message.

---

## 9. Implementation phases

| Phase | Deliverable | Verifiable by |
|---|---|---|
| 1 | Enum + setting + stub `map_generate_space()` filling the map with one terrain; hex rejection | `set generator SPACE`, server starts; hex map refuses |
| 2 | Phases 0–2: sizing, Sol pinned at centre, dart-thrown centres, relaxation | debug dump shows centres with 5–15 tile gaps |
| 3 | Phase 3: concentric ring terrain with wobble | ASCII map dump shows ragged discs |
| 4 | Belts (any ring) and Kuiper belts | visual dump |
| 5 | Space ruleset skeleton — space terrains (`TER_NOT_GENERATED`) + resources, alongside a copied Earth-like set | ruleset loads; `generator=RANDOM` still yields an Earth map |
| 6 | Phase 4: procedural planets and moons + `riches` scaling | body counts per ring match spec at riches 0 / 250 / 1000 |
| 7 | Phase 3b: Sol template + angle sampler | Sol's composition identical across seeds, layout varies, clearances hold |
| 8 | Phase 4b: start positions in Sol | game reaches T1 with all players placed in Sol |
| 9 | Space tileset | playable visually |
| 10 | Tuning (wobble amplitude, gap relaxation, `riches`/`steepness` curves) | sanity script + play |

---

## 10. Testing & validation

* **Deterministic reproduction** — `set mapseed <n>`; `map_fractal_generate()`
  honours `seed_setting` (`mapgen.c:1285`).
* **ASCII dump** — `print_mapgen_map()` (`mapgen.c:1213`) already dumps terrain;
  extend it (or write a throwaway variant) to also mark planets and moons.
* **Invariant checks** (debug-only `space_map_sanity()` at end of Phase 5):
  * every moon has a planet within `sq_map_distance <= 4`;
  * no resource on any `Interstellar Space` / `Near Space` tile;
  * no resource on any `Star`, `Asteroid Belt` or `Kuiper Belt` tile;
  * no two system centres closer than `R_a + R_b + 5`;
  * every system's nearest neighbour is within `R_a + R_b + 15`;
  * `wld.map.num_continents == num_systems` after `assign_continent_numbers()`;
  * every tile with `ptile->resource != NULL` satisfies
    `tile_resource_is_valid()` (catches the §2 half-apply bug);
  * `map_startpos_count() == player_count()`, every startpos on Sol's continent;
  * **Sol composition invariance**: generate at 5 different mapseeds, same size
    — the *multiset* of Sol's bodies (1 star, molten, toxic, 2 rocky with 1 and
    2 moons, 3 gas giants with 5/3/1 moons, ice planet) and their orbital radii
    must be identical, while the angles differ. Layout equality is explicitly
    **not** asserted;
  * **Sol angular separation**: for every pair of Sol bodies, the chord distance
    `d(a,b) >= MAX(clearance_a, clearance_b)` — catches a sampler that fell
    through to its jitter fallback with a bad spacing;
  * every Sol gas giant received all of its specified moons (the fixed template
    must never silently drop a body the way procedural systems may).
* **Stress** — 2–30 players across every map size; assert Phase 0 degrades
  gracefully (shrink `R`, then `N`, then fail cleanly) rather than looping.
* **Ruleset coexistence** — run `generator=RANDOM` and `generator=ISLAND` on the
  space ruleset and confirm no space terrain and no planet resource appears.

---

## 11. Open questions — status

| # | Question | Status |
|---|---|---|
| 1 | Star as terrain vs. resource | **Terrain** (assumed; still worth a confirm) |
| 2 | Is a whole system one continent? | **Yes** — all rings are land class. Belts as impassable barriers is a unit-nativity question, not a continent one |
| 3 | Earth-like terrains kept in the ruleset? | **Resolved: yes**, isolated via `TER_NOT_GENERATED` (§3.1) |
| 4 | Moon output | **Resolved: moons produce output by themselves** |
| 5 | `riches` mapping | **Resolved:** body-density multiplier (§6.1) |
| 6 | Hex topology | **Resolved: rejected**, square/iso-square only |
| 7 | *(new)* Should Sol's ring terrain be richer than a procedural system's, or identical? | Open — affects whether leaving Sol is attractive |
| 8 | *(new)* Sol's ice planet has 0 moons in the template; confirm | Open |

---

## 12. Summary of key code references

| What | Where |
|---|---|
| Generator entry point | `server/generator/mapgen.c:1274` |
| Generator branch to extend | `server/generator/mapgen.c:1321-1374` |
| Startpos mode switch (needs a case) | `server/generator/mapgen.c:1415` |
| Startpos skip guard | `server/generator/mapgen.c:1412` |
| `MAPGEN_FAIR` startpos pattern to copy | `server/generator/mapgen.c:3706-3730` |
| Generator enum | `common/map_types.h:46` |
| Setting names | `server/settings.c:302` |
| Default generator | `common/map.h:676` |
| `riches` range and default | `common/map.h:664-666` |
| Resource placement (suppressed) | `server/generator/mapgen.c:1579` |
| Resource 1-tile spacing rule | `server/generator/mapgen.c:1565` |
| Hut placement (suppressed) | `server/generator/mapgen.c:1538` |
| `tile_set_resource` validity trap | `common/tile.c:344-360` |
| Resource output | `common/city.c:1285` |
| `circle_dxyr_iterate` | `common/map.h:406` |
| Squared-distance metric (hex caveat) | `common/map.c:614` |
| `TER_NOT_GENERATED` skips | `mapgen_utils.c:713, 733, 750`; `mapgen.c:1072` |
| `placed_map` API | `server/generator/mapgen_utils.h` |
| Ocean depth smoothing | `server/generator/mapgen_utils.c:612` |
| Lake regeneration (inert without freshwater) | `server/generator/mapgen_utils.c:350` |
| Start positions (bypassed) | `server/generator/startpos.c:300` |
| `destroy_tmap` non-NULL assert | `server/generator/temperature_map.c:105` |
| Terrain art section lookup | `client/tilespec.c:4138` |
| Terrain sprite names built from section tag | `client/tilespec.c:4165, 4184, 4230` |
| `[tile_*]` section tag → `draw->name` | `client/tilespec.c:2344` |
| Extra style lookup (graphic_str → graphic_alt) | `client/tilespec.c:3870-3875` |

---

## 13. Tileset dataset — art binding

The space tileset (`freeciv/tilesets/space`, its own repo, wired in as a
submodule) is a modified amplio2. It is 2.6-era art carrying amplio2's tag
names, so nothing in it is named after a space concept — but the **art itself
is already space art**. This section binds the two together.

### 13.1 Confirmed art inventory

Every cell below was visually inspected in `space/terrain1.png`, not inferred
from its tag name.

**Ring terrains** (column 0 of `terrain1.spec`; all are starfield diamonds
differing in tint):

| Ring terrain | Borrows art of | Cell | Appearance |
|---|---|---|---|
| `Inner System` | grassland | 2,0 | light grey starfield |
| `Middle System` | plains | 1,0 | mid grey starfield |
| `Outer System` | **arctic** | 7,0 | deep blue starfield — reads as cold, which is why arctic beat tundra |
| `Asteroid Belt` | hills | 4,0 + `hills.spec` | dark starfield + layer-1 relief |
| `Kuiper Belt` | **forest** | 3,0 + `terrain2.spec` | dark starfield + layer-1 clutter |
| `Star` | *(its own cell)* | **10,0** | glowing yellow star — currently mis-tagged `t.l0.inaccessible1` |
| `Near Space` | coast | `water.spec` | — |
| `Interstellar Space` | floor (deep ocean) | `water.spec` | — |

`Star` is the one terrain whose art is already bespoke: cell 10,0, directly
below jungle, is a drawn star that amplio2 uses for "inaccessible".

**Celestial bodies** — nine resource cells, all confirmed to depict what the
mapping claims, and already drawn at two scales so planets read larger than
moons:

| Body | Art tag | Cell | Appearance |
|---|---|---|---|
| Molten Planet | `ts.furs` | 5,4 | large orange-red molten globe |
| Molten Moon | `ts.peat` | 7,2 | small dark red globe |
| Toxic Planet | `ts.arctic_ivory` | 6,2 | large yellow-green globe |
| Toxic Moon | `ts.silk` | 2,4 | small green globe |
| Rocky Planet | `ts.wine` | 3,4 | large brown-orange globe |
| Rocky Moon | `ts.spice` | 7,4 | small tan globe |
| Gas Giant | `ts.whales` | 9,4 | large purple **ringed** giant |
| Ice Planet | `ts.buffalo` | 1,2 | large blue-white ice globe |
| Ice Moon | `ts.grassland_resources` | 11,4 | small pale blue-white globe |

Note `ts.grassland_resources` shares cell 11,4 with `ts.river_resources`; in
Earth tilesets this is the "Resources" shield special, hence its shorthand name.

### 13.2 The key mechanism: `graphic_alt` costs us nothing

Terrain art is **not** looked up sprite-by-sprite. `tileset_setup_tile_type()`
(`client/tilespec.c:4138`) resolves a terrain to one `[tile_*]` section by
trying `graphic_str` then `graphic_alt`; and every sprite name is then built
from **that section's own `tag`** — `draw->name` (`tilespec.c:2344`, used at
`4165`/`4184`/`4230`). Extras resolve the same way, for both the sprite and the
`[extras] styles` entry (`tilespec.c:3870-3875`).

The consequence is the central result of this analysis:

> A space terrain declared as
> `graphic = "inner_system"`, `graphic_alt = "grassland"`
> falls through to `[tile_grassland]` and renders with the full existing
> grassland art — **all 16 match variants, blend sprites and cell sprites
> included** — with **zero changes to the tileset**. The day custom art is
> drawn, adding a `[tile_inner_system]` section plus `t.l0.inner_system1`
> switches it over, with **no ruleset edit**.

So the two obvious approaches are both wrong, and there is a better third:

| Approach | Verdict |
|---|---|
| Ruleset says `graphic = "grassland"` | Works today, but the ruleset is permanently married to amplio tag names and custom art later means editing the ruleset |
| Duplicate every tag as an alias in the specs | Unnecessary — would mean ~16–32 alias lines per terrain, and §13.2 shows none are read |
| **`graphic` = real name, `graphic_alt` = amplio name** | **Chosen.** Correct names now, zero tileset work now, custom art later is purely additive |

### 13.3 Required dataset changes

Almost everything is ruleset-side. The tileset needs **one** substantive edit.

**Tileset (`freeciv-space-tileset` repo) — required:**

1. `space/terrain1.spec` cell 10,0 — add `t.l0.star1` alongside the existing
   `t.l0.inaccessible1` (`duplicates_ok` is already on):
   ```
   10,  0, "t.l0.inaccessible1",
           "t.l0.star1"          ; the art here is a star, not "inaccessible"
   ```
2. `space.tilespec` — add the matching section:
   ```
   [tile_star]
   tag = "star"
   blend_layer = 0
   num_layers = 1
   layer0_match_type = "land"
   ```

   This is the only terrain needing it, because it is the only one whose art
   is not already reachable through an amplio section name.

**Tileset — optional, and not needed by the generator:**

3. The 87 out-of-bounds sprite cells catalogued in §13.5. None block this work.

**Ruleset (`data/space/`) — the real work:**

4. Eight terrains per §3.1, each with `graphic` = its own name and
   `graphic_alt` = the amplio name from §13.1.
5. Nine resources per §3.2, each as the usual `[resource_x]` + `[extra_x]`
   pair, with `graphic` = e.g. `"ts.molten_planet"` and `graphic_alt` = the
   amplio tag from §13.1. All nine fallback tags are already present in the
   `[extras] styles` list, so the style lookup at `tilespec.c:3871` resolves.

### 13.4 What this buys, and what it does not

Rendering works from day one and every name in the ruleset is a real space
name. Two consequences to be aware of:

* **The ring terrains are near-identical, deliberately.** Inner/Middle/Outer
  differ only in tint. This is **not** a defect to be fixed before playtesting
  (OQ12, resolved *no*): a colourful map is explicitly not wanted, and the
  structure a player needs to read is carried by the two belt annuli — whose
  hills and forest layer-1 relief overlays the flat ring tints — plus the
  planet and moon sprites. The muted ring tints are the background those read
  against. Should this prove wrong in play, §13.2 means per-ring art can be
  added later without touching the ruleset.
* **Belts have no bodies yet.** §3.2 gives Asteroid and Kuiper belts no
  resources and §10 asserts they have none. That holds for v1; mineable belts
  are a later stage (OQ9), and the ore art for them already exists (§13.5).

### 13.5 Spare art already in the sheet

Inspecting the unused resource cells turned up more purpose-drawn space art
than the nine bodies need — worth knowing before anyone commissions new art:

| Art tag(s) | Depicts | Status |
|---|---|---|
| `ts.oil`, `ts.arctic_oil` | **black hole with accretion disc** (two copies) | **Reserved** — alternative system centre, later stage (OQ11) |
| `ts.gold`, `ts.iron`, `ts.coal`, `ts.gems` | ore and mineral icons (amplio leftovers) | **Reserved** — mineable belt resources, later stage (OQ9) |
| `ts.oasis`, `ts.wheat` | **habitable blue-green planets** | **Reserved** — Terran planet, distant future (OQ10) |
| `ts.fish` | a second, smaller **star** | Unassigned — binary companion in a multi-star core |
| `ts.pheasant`, `ts.fruit` | green-brown and teal planets | Unassigned — further planet classes |
| `ts.seals` | purple **nebula** wisp | Unassigned — nebula tiles or a decorative extra |
| `ts.horses` | white crystalline shards | Unassigned — comet / ice fragment |
| `ts.tundra_game`, `ts.forest_game` | amplio creature art | Free; not space art |
| `ts.rubber`, `ts.boar`, `ts.berries` | **blank cells** | Free — three empty slots for new art |

Because three of these are reserved for planned features, the sheet should not
be repacked in a way that loses those cells.

### 13.6 Pre-existing out-of-bounds sprite cells

The tileset pairs freeciv-web spec files with narrower amplio-sized art, so 87
declared cells point outside their PNG. The client only complains when a
ruleset actually requests one (which is how `unit.action_decision_want` was
found), so these are latent, not active:

| Spec | Image | Out-of-bounds cells |
|---|---|---|
| `terrain2.spec` | 960x295 | 32 — all `t.l1.desert_*` and `t.l1.swamp_*` |
| `tiles.spec` | 1165x344 | 30 — `city.t_trade_A..L`, `unit.hp_95..5`, `unit.stack2..9` |
| `terrain1.spec` | 960x785 | 9 — every `road.bridge_*` (row 16) |
| `bases.spec` | 863x147 | 7 — radar, quay, castle, castle2, bunker (row 2) |
| `grid.spec` | 583x148 | 5 — usermark, userarea, userspot, pollute (row 3) |
| `cities.spec` | 1068x731 | 4 — coastal and fortification overlays (row 10) |

Relevant to this plan:

* **Safe.** Forest, hills, mountains and jungle all have complete, in-bounds
  16-variant match sets, so Asteroid Belt and Kuiper Belt art works.
  `water.spec` is entirely clean once its per-`[grid_*]` cell geometry is
  accounted for.
* **Latent risk.** `t.l1.desert_*` and `t.l1.swamp_*` are missing, so the
  space ruleset must not give any terrain `graphic_alt = "desert"` or
  `"swamp"` — those two amplio sections would fatal. None of the §13.1
  bindings use them.
* `city.t_trade_A..L` will fatal on any ruleset that produces ≥10 trade on a
  tile. Worth fixing before the ruleset's output values are tuned.

---

## 14. Questions raised by the dataset pass — all resolved

None of these block v1. Three are deferred features whose art already exists,
so the sheet should not be reorganised in a way that loses those cells.

| # | Question | Decision |
|---|---|---|
| 9 | Should Asteroid / Kuiper belts carry mineable resources? | **Yes, later stage.** Not in v1: §3.2 keeps belts resource-free and §10 keeps asserting it. When it lands, use the existing `ts.gold` / `ts.iron` / `ts.coal` / `ts.gems` art and give the "mine-able" flag in §3.1 something to act on |
| 10 | Is a habitable planet a body class? | **Maybe, distant future.** `ts.oasis` / `ts.wheat` stay reserved for it. Would need a ring assignment, a rarity rule and a `riches` interaction — worth doing only if expansion needs a headline prize |
| 11 | Black hole — hazard, or just art? | **Later stage, as an alternative system centre.** `ts.oil` / `ts.arctic_oil` are the art. Implies its own terrain and a generator branch at Phase 3b, since a black-hole system would not have a `Star` core |
| 12 | Do the three ring terrains need distinct art before playtesting? | **No.** The map is intentionally kept muted rather than colourful; the belts' hills and forest relief overlay the ring tints and, with the body sprites, carry all the structure a player needs to read. See §13.4 |

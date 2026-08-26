/***********************************************************************
 Freeciv - Copyright (C) 1996 - A Kjeldberg, L Gregersen, P Unold
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
***********************************************************************/

/*
 * The space map generator: instead of continents and oceans, the map is a
 * void scattered with star systems. Each system is a disc of concentric
 * orbital rings around a central star, and the planets and moons in it are
 * *resources* sitting on those rings rather than terrains of their own -
 * which is what lets one ring hold several kinds of world.
 *
 * At the centre of the map sits Sol, a hand-authored system whose composition
 * is identical in every game (only the orbital angles are drawn per map).
 * Every player starts there. Everything else is procedural.
 *
 * The full design, and the reasoning behind the constraints below, is in
 * docs/space_map_generator_plan.md. Three of them are worth repeating here
 * because they are invisible at the call site:
 *
 *  - This generator never allocates a height map, so it must not touch any
 *    shared helper that calls hmap(): place_terrain(), create_tmap(TRUE),
 *    make_relief(), the river tests. mapgen_utils.c and startpos.c are
 *    hmap-free and are safe to reuse. (plan 7.2)
 *
 *  - It must not call create_tmap() or destroy_tmap(). The framework creates
 *    the temperature map before us and destroys it after; nothing in the
 *    space pipeline reads it. (plan 7.3)
 *
 *  - It creates its own start positions, which is what makes
 *    map_fractal_generate() skip create_start_positions() entirely. (plan 7.1)
 */

#ifdef HAVE_CONFIG_H
#include <fc_config.h>
#endif

#include <math.h>

/* utility */
#include "fcintl.h"
#include "log.h"
#include "rand.h"
#include "shared.h"
#include "support.h"

/* common */
#include "extras.h"
#include "map.h"
#include "terrain.h"
#include "tile.h"

/* generator */
#include "mapgen_utils.h"

#include "space_map.h"


/* Sol must hold every player, so it has a floor independent of map size. */
#define SOL_MIN_RADIUS       10
#define TILES_PER_START      22

/* Procedural systems. */
#define SYS_MIN_RADIUS       4
#define SYS_MAX_RADIUS       9
#define MIN_GAP              5      /* free tiles between two system edges */
#define MAX_GAP              15     /* above this, relaxation pulls closer */
#define PACKING_EFFICIENCY   0.75   /* discs never tile the plane perfectly */
#define MAX_DART_ATTEMPTS    4000
#define MAX_ANGLE_TRIES      200

#define MAX_BELTS            3

/* How far the navigable shallows reach beyond a system's outer edge. */
#define NEAR_SPACE_BAND      3

/* Ring boundaries as a fraction of a system's outer radius. */
#define FRAC_INNER           0.30
#define FRAC_MIDDLE          0.60
#define FRAC_OUTER           0.90

struct space_system {
  struct tile *centre;
  int radius;                   /* R_kuiper - the true outer edge */
  int r_inner, r_middle, r_outer;
  bool is_sol;
  int n_stars;
  int n_belts;
  int belt_r[MAX_BELTS];
  bool has_kuiper;
  float wobble_amp[3];
  float wobble_phase[3];
};

static struct space_system *systems;
static int num_systems;
static bool *body_taken;

/* Resolved once per run, by rule name. */
static struct terrain *ter_star, *ter_inner, *ter_middle, *ter_outer,
                      *ter_asteroid, *ter_kuiper, *ter_near, *ter_deep;
static struct extra_type *res_molten_planet, *res_toxic_planet,
                         *res_rocky_planet, *res_gas_giant, *res_ice_planet,
                         *res_rocky_moon, *res_molten_moon,
                         *res_toxic_moon, *res_ice_moon;

/* The wobble harmonics. Low, coprime-ish frequencies give a ragged edge that
 * still reads as a disc; higher ones just look like noise. */
static const int wobble_freq[3] = { 2, 3, 5 };

/* Sol's fixed composition. Orbits are fractions of R_sol; angles are drawn
 * per map. 'clearance' is how many tiles the body needs to itself, which for
 * a body with moons must cover the neighbourhood its moons will fill. */
struct sol_body {
  double orbit;
  struct extra_type **planet;
  struct extra_type **moon;     /* NULL => moons of mixed type */
  int n_moons;
  int clearance;
};

/* Filled in resolve_ruleset(), since the extra pointers are not constants. */
static struct sol_body sol_template[8];
static int sol_template_size;


/**********************************************************************//**
  Look up everything the generator needs by rule name. Returns FALSE, having
  logged which entry is missing, if the ruleset is not a space ruleset.
**************************************************************************/
static bool resolve_ruleset(void)
{
  int i = 0;

#define RESOLVE_TER(var, name)                                              \
  var = terrain_by_rule_name(name);                                         \
  if (var == NULL) {                                                        \
    log_error(_("The map generator \"Star systems\" needs a terrain named "  \
                "\"%s\", which this ruleset does not define."), name);      \
    return FALSE;                                                           \
  }

#define RESOLVE_RES(var, name)                                              \
  var = extra_type_by_rule_name(name);                                      \
  if (var == NULL) {                                                        \
    log_error(_("The map generator \"Star systems\" needs a resource named " \
                "\"%s\", which this ruleset does not define."), name);      \
    return FALSE;                                                           \
  }

  RESOLVE_TER(ter_star,     "Star");
  RESOLVE_TER(ter_inner,    "Inner System");
  RESOLVE_TER(ter_middle,   "Middle System");
  RESOLVE_TER(ter_outer,    "Outer System");
  RESOLVE_TER(ter_asteroid, "Asteroid Belt");
  RESOLVE_TER(ter_kuiper,   "Kuiper Belt");
  RESOLVE_TER(ter_near,     "Near Space");
  RESOLVE_TER(ter_deep,     "Interstellar Space");

  RESOLVE_RES(res_molten_planet, "Molten Planet");
  RESOLVE_RES(res_toxic_planet,  "Toxic Planet");
  RESOLVE_RES(res_rocky_planet,  "Rocky Planet");
  RESOLVE_RES(res_gas_giant,     "Gas Giant");
  RESOLVE_RES(res_ice_planet,    "Ice Planet");
  RESOLVE_RES(res_rocky_moon,    "Rocky Moon");
  RESOLVE_RES(res_molten_moon,   "Molten Moon");
  RESOLVE_RES(res_toxic_moon,    "Toxic Moon");
  RESOLVE_RES(res_ice_moon,      "Ice Moon");

#undef RESOLVE_TER
#undef RESOLVE_RES

  /* Sol, outermost orbit last. See plan section 3b. */
  sol_template[i++] = (struct sol_body) { 0.20, &res_molten_planet, NULL,             0, 3 };
  sol_template[i++] = (struct sol_body) { 0.28, &res_toxic_planet,  NULL,             0, 3 };
  sol_template[i++] = (struct sol_body) { 0.42, &res_rocky_planet,  &res_rocky_moon,  1, 4 };
  sol_template[i++] = (struct sol_body) { 0.52, &res_rocky_planet,  &res_rocky_moon,  2, 4 };
  sol_template[i++] = (struct sol_body) { 0.72, &res_gas_giant,     NULL,             5, 5 };
  sol_template[i++] = (struct sol_body) { 0.80, &res_gas_giant,     NULL,             3, 5 };
  sol_template[i++] = (struct sol_body) { 0.86, &res_gas_giant,     NULL,             1, 5 };
  sol_template[i++] = (struct sol_body) { 0.90, &res_ice_planet,    &res_ice_moon,    1, 4 };
  sol_template_size = i;

  fc_assert_ret_val(sol_template_size <= ARRAY_SIZE(sol_template), FALSE);

  return TRUE;
}

/**********************************************************************//**
  'riches' redefined as a body-density multiplier: the stock default of 250
  means 1.0. See plan section 6.1 - the per-tile-probability meaning it has in
  add_resources() is useless here, because add_resources() is suppressed and
  our bodies are placed by structural rules rather than per-tile dice.
**************************************************************************/
static double richness(void)
{
  return (double) wld.map.server.riches / (double) MAP_DEFAULT_RICHES;
}

/**********************************************************************//**
  Scale a body count by richness, rounding stochastically so that a
  fractional multiplier gives variety *between* systems instead of rounding
  every system the same way.
**************************************************************************/
static int scale_richness(int base)
{
  double x = base * richness();
  int n = (int) x;

  if (fc_rand(1000) < (int) ((x - n) * 1000)) {
    n++;
  }

  return n;
}

/**********************************************************************//**
  Tile at (dx, dy) from a system centre, or NULL if that falls off the map.
  Goes through map_pos_to_tile() so that wrapping is handled for us.
**************************************************************************/
static struct tile *offset_tile(const struct tile *centre, int dx, int dy)
{
  int cx, cy;

  index_to_map_pos(&cx, &cy, tile_index(centre));

  return map_pos_to_tile(&(wld.map), cx + dx, cy + dy);
}

/**********************************************************************//**
  Tile at polar offset (r, theta) from a system centre.
**************************************************************************/
static struct tile *polar_tile(const struct tile *centre, double r,
                               double theta)
{
  return offset_tile(centre, (int) floor(r * cos(theta) + 0.5),
                     (int) floor(r * sin(theta) + 0.5));
}

/**********************************************************************//**
  The per-system radial perturbation at a given angle, in tiles. Without it
  every system is a mechanically perfect disc.
**************************************************************************/
static double wobble_at(const struct space_system *sys, double theta)
{
  double w = 0.0;
  int k;

  for (k = 0; k < 3; k++) {
    w += sys->wobble_amp[k] * sin(wobble_freq[k] * theta + sys->wobble_phase[k]);
  }

  return w;
}

/**********************************************************************//**
  Fill in the derived geometry of a system: ring radii, wobble, stars, belts.
**************************************************************************/
static void init_system(struct space_system *sys, struct tile *centre,
                        int radius, bool is_sol)
{
  int k, b;

  sys->centre = centre;
  sys->radius = radius;
  sys->is_sol = is_sol;
  sys->r_inner  = (int) floor(FRAC_INNER  * radius + 0.5);
  sys->r_middle = (int) floor(FRAC_MIDDLE * radius + 0.5);
  sys->r_outer  = (int) floor(FRAC_OUTER  * radius + 0.5);

  for (k = 0; k < 3; k++) {
    sys->wobble_amp[k] = 0.6f * (float) fc_rand(100) / 100.0f;
    sys->wobble_phase[k] = (float) (2.0 * M_PI * fc_rand(1000) / 1000.0);
  }

  /* A procedural system may be a binary or trinary. Sol is always single:
   * its composition is fixed, and extra stars would eat the inner ring the
   * players start next to. */
  sys->n_stars = is_sol ? 1 : 1 + fc_rand(3);

  /* 'steepness' redefined: belt count and Kuiper probability. Stock default
   * is 30; treat that as the middle of the range. */
  sys->n_belts = 1 + (wld.map.server.steepness * MAX_BELTS) / 100;
  sys->n_belts = CLIP(0, sys->n_belts, MAX_BELTS);
  sys->has_kuiper = (fc_rand(100) < 30 + wld.map.server.steepness);

  /* Belt radii, at least 2 apart so two belts never merge into a slab. */
  b = 0;
  for (k = 0; k < sys->n_belts * 4 && b < sys->n_belts; k++) {
    int r = 2 + fc_rand(MAX(1, sys->r_outer - 1));
    int j;
    bool ok = TRUE;

    for (j = 0; j < b; j++) {
      if (abs(r - sys->belt_r[j]) < 2) {
        ok = FALSE;
        break;
      }
    }
    if (ok) {
      sys->belt_r[b++] = r;
    }
  }
  sys->n_belts = b;
}

/**********************************************************************//**
  Phase 0. Validate the topology and the ruleset, and decide how many
  systems of what size will fit. Returns FALSE if a space map is impossible.
**************************************************************************/
static bool space_setup(int *r_sol, int *n_procedural, int *r_procedural)
{
  double area_budget, sol_area, sys_area;
  int rs, n, r;

  /* plan 7.5: on hex topologies map_vector_to_sq_distance() squares the hex
   * real-distance, so circle_dxyr_iterate() would carve hexagons, not discs.
   * Rejected outright rather than supporting two geometries. */
  if (current_topo_has_flag(TF_HEX)) {
    log_error(_("The map generator \"Star systems\" does not support hex "
                "topologies. Choose a square topology, or another "
                "generator."));
    return FALSE;
  }

  if (!resolve_ruleset()) {
    return FALSE;
  }

  /* Sol must have room for every player. */
  rs = (int) ceil(sqrt(player_count() * TILES_PER_START / M_PI));
  rs = MAX(rs, SOL_MIN_RADIUS);

  sol_area = M_PI * (rs + MIN_GAP) * (rs + MIN_GAP);
  area_budget = map_num_tiles() * PACKING_EFFICIENCY;

  if (sol_area > area_budget) {
    log_error(_("The map is too small for the \"Star systems\" generator: "
                "the home system alone needs about %d tiles of the %d "
                "available. Use a larger map or fewer players."),
              (int) sol_area, map_num_tiles());
    return FALSE;
  }

  /* Fit the procedural systems into what 'landpercent' asks for, then shrink
   * R before shrinking N: a map of many small systems still plays, a map of
   * two big ones does not. */
  r = SYS_MAX_RADIUS;
  do {
    sys_area = M_PI * (r + MIN_GAP / 2.0) * (r + MIN_GAP / 2.0);
    n = (int) ((map_num_tiles() * wld.map.server.landpercent / 100.0
                - M_PI * rs * rs) / sys_area);

    if (n >= player_count() || r <= SYS_MIN_RADIUS) {
      break;
    }
    r--;
  } while (r >= SYS_MIN_RADIUS);

  n = MAX(n, 0);

  /* Never promise more than will physically fit. */
  while (n > 0 && sol_area + n * sys_area > area_budget) {
    n--;
  }

  log_verbose("Space generator: R_sol=%d, %d procedural systems of R<=%d "
              "on %d tiles", rs, n, r, map_num_tiles());

  if (n == 0) {
    log_normal(_("The \"Star systems\" map has room for the home system "
                 "only. Expansion will be impossible; consider a larger "
                 "map or a higher 'landpercent'."));
  }

  *r_sol = rs;
  *n_procedural = n;
  *r_procedural = r;

  return TRUE;
}

/**********************************************************************//**
  Phase 1. Place system centres: Sol pinned at the map centre, the rest by
  dart throwing, then relaxed so the gaps land in the 5-15 tile band rather
  than merely "at least 5".
**************************************************************************/
static void place_centres(int r_sol, int n_procedural, int r_procedural)
{
  int attempts;

  systems = fc_calloc(n_procedural + 1, sizeof(*systems));
  num_systems = 0;

  init_system(&systems[num_systems++],
              native_pos_to_tile(&(wld.map), wld.map.xsize / 2,
                                 wld.map.ysize / 2),
              r_sol, TRUE);

  for (attempts = 0;
       attempts < MAX_DART_ATTEMPTS && num_systems <= n_procedural;
       attempts++) {
    struct tile *ptile = rand_map_pos(&(wld.map));
    int radius = SYS_MIN_RADIUS
      + fc_rand(MAX(1, r_procedural - SYS_MIN_RADIUS + 1));
    bool ok = TRUE;
    int i;

    /* real_map_distance() rather than raw coordinates, so a system near the
     * seam of a wrapping map is not wrongly considered far from one on the
     * other side. */
    for (i = 0; i < num_systems; i++) {
      if (real_map_distance(ptile, systems[i].centre)
          < radius + systems[i].radius + MIN_GAP) {
        ok = FALSE;
        break;
      }
    }

    if (ok) {
      init_system(&systems[num_systems++], ptile, radius, FALSE);
    }
  }

  log_verbose("Space generator: placed %d systems in %d dart throws",
              num_systems, attempts);
}

/**********************************************************************//**
  Phase 2. Everything is void until a system is carved into it.
**************************************************************************/
static void fill_background(void)
{
  whole_map_iterate(&(wld.map), ptile) {
    tile_set_terrain(ptile, ter_deep);
  } whole_map_iterate_end;
}

/**********************************************************************//**
  Is this tile on one of the system's belt annuli?
**************************************************************************/
static bool on_belt(const struct space_system *sys, double r_eff, double theta)
{
  int k;

  for (k = 0; k < sys->n_belts; k++) {
    if (fabs(r_eff - sys->belt_r[k]) < 0.5) {
      /* 60-100% angular coverage: a deterministic function of the angle, so
       * the same tile always gets the same answer without a second array. */
      double gap = 0.5 + 0.5 * sin(7.0 * theta + sys->wobble_phase[k % 3]);

      return gap > (0.4 - wld.map.server.steepness / 250.0);
    }
  }

  return FALSE;
}

/**********************************************************************//**
  Phase 3. Carve one system's concentric rings, its star(s) and its belts.
**************************************************************************/
static void carve_system(struct space_system *sys)
{
  int star_placed = 0;

  circle_dxyr_iterate(&(wld.map), sys->centre, sys->radius * sys->radius,
                      ptile, dx, dy, dr) {
    double theta = atan2((double) dy, (double) dx);
    double r_eff = sqrt((double) dr) - wobble_at(sys, theta);
    struct terrain *pterr;

    if (r_eff > sys->radius) {
      continue;
    }

    if (r_eff <= sys->r_inner) {
      pterr = ter_inner;
    } else if (r_eff <= sys->r_middle) {
      pterr = ter_middle;
    } else if (r_eff <= sys->r_outer) {
      pterr = ter_outer;
    } else if (sys->has_kuiper) {
      pterr = ter_kuiper;
    } else {
      pterr = ter_outer;
    }

    if (pterr != ter_kuiper && on_belt(sys, r_eff, theta)) {
      pterr = ter_asteroid;
    }

    tile_set_terrain(ptile, pterr);
    map_set_placed(ptile);
  } circle_dxyr_iterate_end;

  /* The star(s) last, so no ring pass can overwrite them. A star is
   * impassable, so a multiple star system has a correspondingly larger hole
   * at its centre. */
  circle_dxyr_iterate(&(wld.map), sys->centre, 2, ptile, dx, dy, dr) {
    if (star_placed >= sys->n_stars) {
      break;
    }
    tile_set_terrain(ptile, ter_star);
    star_placed++;
  } circle_dxyr_iterate_end;
}

/**********************************************************************//**
  Place one resource, if the tile is free and the ruleset allows that body on
  that terrain. The terrain check is what confines each kind of world to its
  ring: it reads the 'resources' line of the terrain, so the ring gating lives
  in the ruleset rather than being duplicated here.
**************************************************************************/
static bool try_place_body(struct tile *ptile, struct extra_type *res)
{
  if (ptile == NULL || body_taken[tile_index(ptile)]) {
    return FALSE;
  }
  if (!terrain_has_resource(tile_terrain(ptile), res)) {
    return FALSE;
  }

  tile_set_resource(ptile, res);
  body_taken[tile_index(ptile)] = TRUE;

  return TRUE;
}

/**********************************************************************//**
  Hang up to n_moons moons around a planet, searching outward from it.
  Returns how many were actually placed - a planet in a crowded ring simply
  gets fewer, which is preferable to failing the map.
**************************************************************************/
static int place_moons(struct tile *planet, struct extra_type **types,
                       int n_types, int n_moons)
{
  int placed = 0;

  if (n_moons <= 0) {
    return 0;
  }

  /* sq_map_distance <= 4 is the 8 neighbours plus the ring beyond, which is
   * what a gas giant with up to 8 moons needs. */
  circle_dxyr_iterate(&(wld.map), planet, 4, ptile, dx, dy, dr) {
    if (placed >= n_moons) {
      break;
    }
    if (dr == 0) {
      continue;                 /* the planet itself */
    }
    if (try_place_body(ptile, types[fc_rand(n_types)])) {
      placed++;
    }
  } circle_dxyr_iterate_end;

  return placed;
}

/**********************************************************************//**
  Pick a free tile of the given terrain in a system, at least min_sep from
  every body already placed there. Returns NULL if none was found.
**************************************************************************/
static struct tile *find_body_site(struct space_system *sys,
                                   struct terrain *ring, int min_sep)
{
  struct tile *best = NULL;
  int seen = 0;

  /* Reservoir sampling over the ring's free tiles: one pass, no candidate
   * array, and every eligible tile equally likely. */
  circle_dxyr_iterate(&(wld.map), sys->centre, sys->radius * sys->radius,
                      ptile, dx, dy, dr) {
    bool crowded = FALSE;

    if (tile_terrain(ptile) != ring || body_taken[tile_index(ptile)]) {
      continue;
    }

    circle_dxyr_iterate(&(wld.map), ptile, min_sep * min_sep, ptile2,
                        dx2, dy2, dr2) {
      if (body_taken[tile_index(ptile2)]
          && tile_resource(ptile2) != NULL) {
        crowded = TRUE;
        break;
      }
    } circle_dxyr_iterate_end;

    if (crowded) {
      continue;
    }

    seen++;
    if (fc_rand(seen) == 0) {
      best = ptile;
    }
  } circle_dxyr_iterate_end;

  return best;
}

/**********************************************************************//**
  Phase 4. Populate one procedural system with planets and moons.
**************************************************************************/
static void populate_system(struct space_system *sys)
{
  struct extra_type *inner_types[2];
  struct extra_type *giant_moons[4];
  int n, i;

  inner_types[0] = res_molten_planet;
  inner_types[1] = res_toxic_planet;

  giant_moons[0] = res_molten_moon;
  giant_moons[1] = res_toxic_moon;
  giant_moons[2] = res_rocky_moon;
  giant_moons[3] = res_ice_moon;

  /* Inner ring: hot, moonless worlds. */
  n = CLIP(0, scale_richness(fc_rand(4)), 3);
  for (i = 0; i < n; i++) {
    struct tile *ptile = find_body_site(sys, ter_inner, 3);

    if (ptile != NULL) {
      try_place_body(ptile, inner_types[fc_rand(2)]);
    }
  }

  /* Middle ring: rocky worlds with a moon or three. */
  n = CLIP(0, scale_richness(fc_rand(4)), 3);
  for (i = 0; i < n; i++) {
    struct tile *ptile = find_body_site(sys, ter_middle, 4);

    if (ptile != NULL && try_place_body(ptile, res_rocky_planet)) {
      place_moons(ptile, &res_rocky_moon, 1,
                  CLIP(1, scale_richness(1 + fc_rand(3)), 3));
    }
  }

  /* Outer ring: gas giants with big mixed moon systems... */
  n = CLIP(0, scale_richness(fc_rand(3)), 2);
  for (i = 0; i < n; i++) {
    struct tile *ptile = find_body_site(sys, ter_outer, 5);

    if (ptile != NULL && try_place_body(ptile, res_gas_giant)) {
      place_moons(ptile, giant_moons, 4,
                  CLIP(3, scale_richness(3 + fc_rand(6)), 8));
    }
  }

  /* ...and ice worlds. */
  n = CLIP(0, scale_richness(fc_rand(3)), 2);
  for (i = 0; i < n; i++) {
    struct tile *ptile = find_body_site(sys, ter_outer, 4);

    if (ptile != NULL && try_place_body(ptile, res_ice_planet)) {
      place_moons(ptile, &res_ice_moon, 1,
                  CLIP(0, scale_richness(fc_rand(3)), 2));
    }
  }
}

/**********************************************************************//**
  Phase 3b/4b. Sol's composition is fixed; only the angles are drawn. Bodies
  are placed outermost first, because the outer orbits have the largest
  clearance and the least angular slack.
**************************************************************************/
static void populate_sol(struct space_system *sys)
{
  double angle[ARRAY_SIZE(sol_template)];
  double radius[ARRAY_SIZE(sol_template)];
  struct extra_type *giant_moons[4];
  int i, j, tries;

  giant_moons[0] = res_molten_moon;
  giant_moons[1] = res_toxic_moon;
  giant_moons[2] = res_rocky_moon;
  giant_moons[3] = res_ice_moon;

  for (i = 0; i < sol_template_size; i++) {
    radius[i] = sol_template[i].orbit * sys->radius;
  }

  /* Rejection sampling on the chord distance. Two bodies on *different*
   * orbits are further apart than their angular difference alone suggests,
   * so the constraint is checked as a real distance rather than as an angle:
   *   d = sqrt(r_a^2 + r_b^2 - 2 r_a r_b cos(theta_a - theta_b)) */
  for (i = sol_template_size - 1; i >= 0; i--) {
    bool ok = FALSE;

    for (tries = 0; tries < MAX_ANGLE_TRIES && !ok; tries++) {
      angle[i] = 2.0 * M_PI * fc_rand(3600) / 3600.0;
      ok = TRUE;

      for (j = i + 1; j < sol_template_size; j++) {
        double d = sqrt(radius[i] * radius[i] + radius[j] * radius[j]
                        - 2.0 * radius[i] * radius[j]
                          * cos(angle[i] - angle[j]));

        if (d < MAX(sol_template[i].clearance, sol_template[j].clearance)) {
          ok = FALSE;
          break;
        }
      }
    }

    if (!ok) {
      /* Deterministic even spacing plus a jitter. Worse-looking than a
       * successful draw, but it never leaves a body unplaced. */
      angle[i] = 2.0 * M_PI * i / sol_template_size
        + (fc_rand(200) - 100) / 1000.0;
      log_verbose("Space generator: Sol body %d fell back to even spacing", i);
    }
  }

  for (i = 0; i < sol_template_size; i++) {
    const struct sol_body *body = &sol_template[i];
    struct tile *ptile = NULL;
    double dr;

    /* The ideal tile may have been taken by the ring wobble (which can push
     * an orbit into the Kuiper annulus) or by a belt, so search outward from
     * it. Sol's composition is fixed and must not silently lose a body the
     * way a procedural system may. */
    for (dr = 0.0; dr <= 2.0 && ptile == NULL; dr += 0.5) {
      double d;

      for (d = 0.0; d <= 0.5 && ptile == NULL; d += 0.1) {
        struct tile *cand = polar_tile(sys->centre, radius[i] - dr,
                                       angle[i] + d);

        if (cand != NULL && try_place_body(cand, *body->planet)) {
          ptile = cand;
        }
      }
    }

    if (ptile == NULL) {
      log_error("Space generator: Sol body %d (%s) could not be placed "
                "at r=%.1f", i, extra_rule_name(*body->planet), radius[i]);
      continue;
    }

    if (body->n_moons > 0) {
      if (body->moon != NULL) {
        place_moons(ptile, body->moon, 1, body->n_moons);
      } else {
        place_moons(ptile, giant_moons, 4, body->n_moons);
      }
    }
  }
}

/**********************************************************************//**
  Phase 4b. Every player starts in Sol, spread by angle around the band
  between the inner and middle rings.

  Doing this ourselves is what makes map_fractal_generate() skip
  create_start_positions() (it guards on map_startpos_count() == 0), which in
  turn means the TER_STARTER filter and the "at least player_count() + 3
  continents" rule never apply to a space map.
**************************************************************************/
static bool place_start_positions(struct space_system *sol)
{
  struct tile **chosen = fc_calloc(player_count(), sizeof(*chosen));
  double band = sol->r_inner + (sol->r_middle - sol->r_inner) / 2.0;
  int placed = 0;
  int i;

  for (i = 0; i < player_count(); i++) {
    double theta = 2.0 * M_PI * i / player_count();
    struct tile *found = NULL;
    double dr;

    /* Walk outward from the ideal spot until a usable tile turns up. */
    for (dr = 0.0; dr <= sol->r_outer - band && found == NULL; dr += 1.0) {
      double d;

      for (d = -0.3; d <= 0.3 && found == NULL; d += 0.15) {
        struct tile *ptile = polar_tile(sol->centre, band + dr, theta + d);
        struct terrain *pterr;
        bool too_close = FALSE;
        int j;

        if (ptile == NULL) {
          continue;
        }
        pterr = tile_terrain(ptile);
        if (pterr != ter_inner && pterr != ter_middle && pterr != ter_outer) {
          continue;             /* star, belt or void */
        }
        if (tile_resource(ptile) != NULL) {
          continue;             /* don't start on top of a planet */
        }

        for (j = 0; j < placed; j++) {
          if (real_map_distance(ptile, chosen[j]) < 3) {
            too_close = TRUE;
            break;
          }
        }
        if (!too_close) {
          found = ptile;
        }
      }
    }

    if (found == NULL) {
      log_error(_("The \"Star systems\" generator could not fit %d start "
                  "positions into the home system."), player_count());
      free(chosen);
      return FALSE;
    }

    chosen[placed++] = found;
  }

  for (i = 0; i < placed; i++) {
    struct startpos *psp = map_startpos_new(chosen[i]);

    /* A fresh startpos allows no nation at all until told otherwise. */
    startpos_allows_all(psp);
  }

  free(chosen);

  return TRUE;
}

/**********************************************************************//**
  Paint the shallow band around each system.

  The stock pipeline would normally do this in smooth_water_depth(), but that
  reaches for its replacement terrain through pick_ocean(), which filters
  TER_NOT_GENERATED (mapgen_utils.c:555) - so on a space ruleset it would
  quietly swap the void for the ruleset's Earth ocean. map_fractal_generate()
  therefore skips it for us, and we do the one thing it was wanted for here.
**************************************************************************/
static void paint_near_space(void)
{
  int i;

  for (i = 0; i < num_systems; i++) {
    int r = systems[i].radius + NEAR_SPACE_BAND;

    circle_dxyr_iterate(&(wld.map), systems[i].centre, r * r, ptile,
                        dx, dy, dr) {
      if (tile_terrain(ptile) == ter_deep) {
        tile_set_terrain(ptile, ter_near);
      }
    } circle_dxyr_iterate_end;
  }
}

/**********************************************************************//**
  Log which settings this generator quietly ignores, so a player who set them
  is not left wondering why nothing happened.
**************************************************************************/
static void warn_ignored_settings(void)
{
  if (wld.map.server.huts > 0) {
    log_normal(_("The \"Star systems\" generator places no huts; the 'huts' "
                 "setting is ignored."));
  }
  if (wld.map.server.startpos != MAPSTARTPOS_DEFAULT) {
    log_normal(_("The \"Star systems\" generator places all players in the "
                 "home system; the 'startpos' setting is ignored."));
  }
}

/**********************************************************************//**
  Generate a map of star systems. See the file comment and
  docs/space_map_generator_plan.md.
**************************************************************************/
bool map_generate_space(void)
{
  int r_sol, n_procedural, r_procedural;
  bool ok = TRUE;
  int i;

  systems = NULL;
  num_systems = 0;
  body_taken = NULL;

  if (!space_setup(&r_sol, &n_procedural, &r_procedural)) {
    return FALSE;
  }

  warn_ignored_settings();

  body_taken = fc_calloc(MAP_INDEX_SIZE, sizeof(*body_taken));
  create_placed_map();

  place_centres(r_sol, n_procedural, r_procedural);
  fill_background();

  for (i = 0; i < num_systems; i++) {
    carve_system(&systems[i]);
  }

  /* Bodies only after every system's terrain exists: try_place_body() reads
   * the terrain to decide whether a body belongs there. */
  populate_sol(&systems[0]);
  for (i = 1; i < num_systems; i++) {
    populate_system(&systems[i]);
  }

  paint_near_space();

  if (log_do_output_for_level(LOG_VERBOSE)) {
    int bodies = 0;

    whole_map_iterate(&(wld.map), ptile) {
      if (tile_resource(ptile) != NULL) {
        bodies++;
      }
    } whole_map_iterate_end;
    log_verbose("Space generator: %d systems, %d celestial bodies",
                num_systems, bodies);
  }

  if (!place_start_positions(&systems[0])) {
    ok = FALSE;
  }

  /* Suppress the stock post-passes. add_resources() would ignore our
   * structure and its 1-tile minimum spacing makes a moon beside its planet
   * impossible; make_huts() has nothing to place. */
  wld.map.server.have_resources = TRUE;
  wld.map.server.have_huts = TRUE;

  destroy_placed_map();
  free(body_taken);
  body_taken = NULL;
  free(systems);
  systems = NULL;
  num_systems = 0;

  return ok;
}

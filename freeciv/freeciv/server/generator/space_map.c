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
 * Every system is generated the same way; none is hand-authored. Players are
 * placed one to a system, and the systems left over are the expansion
 * targets.
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


/* A system smaller than this is not worth generating, so space_setup() aims
 * for it before it starts shrinking. Upper size is whatever fits the budget,
 * not a constant: anchoring the draw to an absolute floor made every system
 * small even on maps with room to spare. SYS_MIN_FRAC keeps the spread
 * narrow so systems stay comparable to one another, and SYS_ABS_MIN is the
 * floor for maps too cramped to honour the fraction. */
#define SYS_MIN_RADIUS       8
#define SYS_MIN_FRAC         0.70
#define SYS_ABS_MIN          4
#define MIN_GAP              3      /* free tiles between two system edges */

/* A system must clear these to be eligible to hold a player. Generation does
 * not know which systems will: homes are chosen from the finished set, which
 * is what keeps one generator honest for every star. The body and workable
 * floors are what stop a trinary or a barren system from crippling whoever
 * lands in it.
 *
 * The radius floor is a fraction of the largest system the map actually got,
 * not an absolute: space_setup() shrinks the radius to fit the budget, so any
 * constant large enough to mean "roomy" on a big map excludes every system on
 * a small one - and excluding every system is how this generator fails. */
#define HOME_MIN_RADIUS_FRAC 0.80
#define HOME_MIN_BODIES      1
#define HOME_MIN_WORKABLE    12
#define HOME_TYPICAL_FRAC    0.85
#define PACKING_EFFICIENCY   0.75   /* discs never tile the plane perfectly */
/* Per system, not for the whole map: place_centres() places the largest disc
 * first and gives each one its own budget. */
#define MAX_DART_ATTEMPTS    600
#define MAX_ANGLE_TRIES      200

#define MAX_BELTS            3

/* How the sky is divided between single, binary and trinary systems, in per
 * cent; the trinary share is whatever the two below leave over. Single stars
 * dominate deliberately - a multiple system should be a thing a player
 * remarks on when they find one. */
#define PCT_SINGLE_STAR      65
#define PCT_BINARY_STAR      25

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


/**********************************************************************//**
  Look up everything the generator needs by rule name. Returns FALSE, having
  logged which entry is missing, if the ruleset is not a space ruleset.
**************************************************************************/
static bool resolve_ruleset(void)
{
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
                        int radius)
{
  int k, b, roll;

  sys->centre = centre;
  sys->radius = radius;
  sys->r_inner  = (int) floor(FRAC_INNER  * radius + 0.5);
  sys->r_middle = (int) floor(FRAC_MIDDLE * radius + 0.5);
  sys->r_outer  = (int) floor(FRAC_OUTER  * radius + 0.5);

  for (k = 0; k < 3; k++) {
    sys->wobble_amp[k] = 0.6f * (float) fc_rand(100) / 100.0f;
    sys->wobble_phase[k] = (float) (2.0 * M_PI * fc_rand(1000) / 1000.0);
  }

  /* Most systems are a single star; a binary is uncommon and a trinary rare.
   * The weighting matters: drawing uniformly from {1, 2, 3} - which is what
   * '1 + fc_rand(3)' did - made two thirds of the sky multiple, so a binary
   * read as the norm rather than as something worth noticing. A crowded
   * centre also eats into the inner ring, which is why choose_home_systems()
   * checks how much workable room a system actually has before putting a
   * player in it. */
  roll = fc_rand(100);
  sys->n_stars = (roll < PCT_SINGLE_STAR ? 1
                  : (roll < PCT_SINGLE_STAR + PCT_BINARY_STAR ? 2 : 3));

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
static bool space_setup(int *n_systems, int *r_max)
{
  double area_budget, sys_area;
  int target, n, r;

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

  area_budget = map_num_tiles() * PACKING_EFFICIENCY;

  /* One system per player, plus room to expand into. The margin is what
   * makes the map something other than a set of sealed starting boxes. */
  target = player_count() + MAX(1, player_count() / 2);

  /* Fit the systems into what 'landpercent' asks for, shrinking R before
   * giving up on N: a map of many small systems still plays, a map of two
   * big ones does not. N is whatever the budget yields at the radius that
   * first reaches the target, so a roomy map simply gets more systems. */
  r = SYS_MIN_RADIUS;
  do {
    sys_area = M_PI * (r + MIN_GAP / 2.0) * (r + MIN_GAP / 2.0);
    n = (int) (map_num_tiles() * wld.map.server.landpercent / 100.0
               / sys_area);

    if (n >= target || r <= SYS_ABS_MIN) {
      break;
    }
    r--;
  } while (r >= SYS_ABS_MIN);

  n = MAX(n, 0);

  /* Never promise more than will physically fit. */
  while (n > 0 && n * sys_area > area_budget) {
    n--;
  }

  log_verbose("Space generator: %d systems of R<=%d on %d tiles "
              "(%d players, target %d)",
              n, r, map_num_tiles(), player_count(), target);

  if (n < player_count()) {
    log_error(_("The map is too small for the \"Star systems\" generator: "
                "it has room for %d star systems but there are %d players, "
                "and each player needs a system of their own. Use a larger "
                "map, a higher 'landpercent', or fewer players."),
              n, player_count());
    return FALSE;
  }

  if (n == player_count()) {
    log_normal(_("The \"Star systems\" map has room for one system per "
                 "player and none to spare. Expansion will be impossible; "
                 "consider a larger map or a higher 'landpercent'."));
  }

  *n_systems = n;
  *r_max = r;

  return TRUE;
}

/**********************************************************************//**
  Sort helper: descending, so the largest disc is placed first.
**************************************************************************/
static int cmp_radius_desc(const void *a, const void *b)
{
  return *(const int *) b - *(const int *) a;
}

/**********************************************************************//**
  Would a system of this radius centred here clear every system already
  placed? Uses real_map_distance() rather than raw coordinates, so a system
  near the seam of a wrapping map is not wrongly considered far from one on
  the other side.
**************************************************************************/
static bool centre_is_clear(const struct tile *ptile, int radius)
{
  int i;

  for (i = 0; i < num_systems; i++) {
    if (real_map_distance(ptile, systems[i].centre)
        < radius + systems[i].radius + MIN_GAP) {
      return FALSE;
    }
  }

  return TRUE;
}

/**********************************************************************//**
  Phase 1. Place system centres by dart throwing.

  The radii are rolled up front and placed largest first. Rolling a fresh
  radius per throw - the obvious way, and what this did originally - packs
  badly: random sequential adsorption of discs saturates near 54% coverage,
  and once the map is that full a late throw that happens to roll a large
  radius fails over and over while a small one would still have fitted. Big
  discs first, each with its own attempt budget and shrinking by a tile when
  it cannot be placed, gets much closer to the requested count.
**************************************************************************/
static void place_centres(int n_systems, int r_max)
{
  int *radii;
  int r_min, i;

  systems = fc_calloc(n_systems, sizeof(*systems));
  num_systems = 0;

  if (n_systems <= 0) {
    return;
  }

  r_min = MAX(SYS_ABS_MIN, (int) floor(SYS_MIN_FRAC * r_max + 0.5));
  r_min = MIN(r_min, r_max);

  radii = fc_malloc(n_systems * sizeof(*radii));
  for (i = 0; i < n_systems; i++) {
    radii[i] = r_min + fc_rand(r_max - r_min + 1);
  }
  qsort(radii, n_systems, sizeof(*radii), cmp_radius_desc);

  for (i = 0; i < n_systems; i++) {
    int radius = radii[i];

    while (radius >= SYS_ABS_MIN) {
      struct tile *found = NULL;
      int attempts;

      for (attempts = 0; attempts < MAX_DART_ATTEMPTS; attempts++) {
        struct tile *ptile = rand_map_pos(&(wld.map));

        if (centre_is_clear(ptile, radius)) {
          found = ptile;
          break;
        }
      }

      if (found != NULL) {
        init_system(&systems[num_systems++], found, radius);
        break;
      }

      /* Nowhere left for a disc this big. Try one tile smaller before
       * giving up on it entirely. */
      radius--;
    }
  }

  free(radii);

  log_verbose("Space generator: placed %d of %d systems, radii %d-%d",
              num_systems, n_systems, r_min, r_max);

  if (num_systems < player_count()) {
    log_normal(_("The \"Star systems\" map fitted only %d systems for %d "
                 "players. Expansion will be tight; consider a larger map "
                 "or a higher 'landpercent'."),
               num_systems, player_count());
  }
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
  Phase 4. Populate one system with planets and moons.
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
  How many tiles of this system a colony could actually work: ring tiles
  with no body already on them. A trinary eats into the inner ring and a
  crowded population leaves little bare ground, and either can make a system
  a poor place to wake up in.
**************************************************************************/
static int workable_tiles(const struct space_system *sys)
{
  int n = 0;

  circle_dxyr_iterate(&(wld.map), sys->centre, sys->radius * sys->radius,
                      ptile, dx, dy, dr) {
    struct terrain *pterr = tile_terrain(ptile);

    if ((pterr == ter_inner || pterr == ter_middle || pterr == ter_outer)
        && tile_resource(ptile) == NULL) {
      n++;
    }
  } circle_dxyr_iterate_end;

  return n;
}

/**********************************************************************//**
  How many celestial bodies this system holds.
**************************************************************************/
static int body_count(const struct space_system *sys)
{
  int n = 0;

  circle_dxyr_iterate(&(wld.map), sys->centre, sys->radius * sys->radius,
                      ptile, dx, dy, dr) {
    if (tile_resource(ptile) != NULL) {
      n++;
    }
  } circle_dxyr_iterate_end;

  return n;
}

/**********************************************************************//**
  Phase 4b, first half. Choose which systems the players wake up in.

  Every system was generated by the same code, so a home is *selected*, not
  built: this runs after population and reads the finished article. Three
  stages - reject systems nobody could live in, prefer the ones closest to
  typical so no player draws a visibly better system than their neighbour,
  then spread the picks as far apart as the map allows.

  Returns the number chosen, which is player_count() on success. Fills
  home[] with indices into systems[].
**************************************************************************/
static int choose_home_systems(int *home)
{
  int *eligible = fc_malloc(num_systems * sizeof(*eligible));
  int *pool = fc_malloc(num_systems * sizeof(*pool));
  int *work = fc_malloc(num_systems * sizeof(*work));
  int *sorted = fc_malloc(num_systems * sizeof(*sorted));
  int n_eligible = 0, n_pool = 0, n_home = 0;
  int median, r_best = 0, r_floor, i, j;
  double frac;

  for (i = 0; i < num_systems; i++) {
    r_best = MAX(r_best, systems[i].radius);
  }
  r_floor = MAX(SYS_ABS_MIN, (int) floor(HOME_MIN_RADIUS_FRAC * r_best));

  for (i = 0; i < num_systems; i++) {
    work[i] = workable_tiles(&systems[i]);

    if (systems[i].radius >= r_floor
        && body_count(&systems[i]) >= HOME_MIN_BODIES
        && work[i] >= HOME_MIN_WORKABLE) {
      eligible[n_eligible++] = i;
    }
  }

  if (n_eligible < player_count()) {
    log_error(_("The \"Star systems\" generator produced only %d systems fit "
                "to start in, for %d players."),
              n_eligible, player_count());
    log_verbose("Space generator: %d systems, largest R=%d, home floor R=%d",
                num_systems, r_best, r_floor);
    free(eligible);
    free(pool);
    free(work);
    free(sorted);
    return 0;
  }

  /* Typicality band around the median, widened until it holds enough
   * candidates. The eligibility floor above is never relaxed - a start has
   * to be viable before it is allowed to be merely unusual.
   *
   * The measure is workable tiles, not radius. Radius is the wrong proxy:
   * space_setup() draws every radius from a deliberately narrow range, so
   * banding on it rejects nothing, while the room a system actually offers
   * still varies severalfold with how many stars crowd the centre, how many
   * belts cut through the rings and how much of the ground bodies already
   * occupy. Banding is two-sided for the same reason the floor exists: a
   * home twice as rich as its neighbours is as unfair as one half as rich. */
  for (i = 0; i < n_eligible; i++) {
    sorted[i] = work[eligible[i]];
  }
  qsort(sorted, n_eligible, sizeof(*sorted), cmp_radius_desc);
  median = sorted[n_eligible / 2];

  for (frac = HOME_TYPICAL_FRAC; ; frac -= 0.05) {
    n_pool = 0;
    for (i = 0; i < n_eligible; i++) {
      if (work[eligible[i]] >= frac * median
          && (frac <= 0.0 || work[eligible[i]] <= median / frac)) {
        pool[n_pool++] = eligible[i];
      }
    }
    if (n_pool >= player_count() || frac <= 0.0) {
      break;
    }
  }

  /* Farthest-point sampling: seed at random, then repeatedly take whichever
   * candidate is furthest from everything picked so far. */
  i = fc_rand(n_pool);
  home[n_home++] = pool[i];
  pool[i] = pool[--n_pool];

  while (n_home < player_count() && n_pool > 0) {
    int best = 0, best_d = -1;

    for (i = 0; i < n_pool; i++) {
      int nearest = -1;

      for (j = 0; j < n_home; j++) {
        int d = real_map_distance(systems[pool[i]].centre,
                                  systems[home[j]].centre);

        if (nearest < 0 || d < nearest) {
          nearest = d;
        }
      }
      if (nearest > best_d) {
        best_d = nearest;
        best = i;
      }
    }

    home[n_home++] = pool[best];
    pool[best] = pool[--n_pool];
  }

  log_verbose("Space generator: %d homes chosen from %d eligible, %d typical "
              "(median %d workable tiles, band %.2f), %d systems left empty",
              n_home, n_eligible, n_pool + n_home, median, frac,
              num_systems - n_home);

  free(eligible);
  free(pool);
  free(work);
  free(sorted);

  return n_home;
}

/**********************************************************************//**
  Phase 4b, second half. One start position per home system.

  Doing this ourselves is what makes map_fractal_generate() skip
  create_start_positions() (it guards on map_startpos_count() == 0), which in
  turn means the TER_STARTER filter and the "at least player_count() + 3
  continents" rule never apply to a space map.
**************************************************************************/
static bool place_start_positions(const int *home, int n_home)
{
  int i;

  for (i = 0; i < n_home; i++) {
    struct space_system *sys = &systems[home[i]];
    struct tile *found = NULL;
    int pass;

    /* The temperate ring first - that is where the ruleset's helptext says
     * players start, and it is the only ring that feeds a colony without
     * being irrigated first. Outer before Inner on the fallback, because
     * Inner is the ring the stars crowd. */
    for (pass = 0; pass < 3 && found == NULL; pass++) {
      struct terrain *want = (pass == 0 ? ter_middle
                              : pass == 1 ? ter_outer : ter_inner);
      int seen = 0;

      circle_dxyr_iterate(&(wld.map), sys->centre, sys->radius * sys->radius,
                          ptile, dx, dy, dr) {
        if (tile_terrain(ptile) == want && tile_resource(ptile) == NULL) {
          /* Reservoir sample, so a start is not always at the same bearing
           * from its star. */
          seen++;
          if (fc_rand(seen) == 0) {
            found = ptile;
          }
        }
      } circle_dxyr_iterate_end;
    }

    if (found == NULL) {
      log_error(_("The \"Star systems\" generator could not find a start "
                  "tile in the system chosen for player %d."), i + 1);
      return FALSE;
    }

    (void) map_startpos_new(found);
  }

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
    log_normal(_("The \"Star systems\" generator gives every player a star "
                 "system of their own; the 'startpos' setting is ignored."));
  }
}

/**********************************************************************//**
  Generate a map of star systems. See the file comment and
  docs/space_map_generator_plan.md.
**************************************************************************/
bool map_generate_space(void)
{
  int n_systems, r_max, n_home;
  int *home;
  bool ok = TRUE;
  int i;

  systems = NULL;
  num_systems = 0;
  body_taken = NULL;

  if (!space_setup(&n_systems, &r_max)) {
    return FALSE;
  }

  warn_ignored_settings();

  body_taken = fc_calloc(MAP_INDEX_SIZE, sizeof(*body_taken));
  create_placed_map();

  place_centres(n_systems, r_max);
  fill_background();

  for (i = 0; i < num_systems; i++) {
    carve_system(&systems[i]);
  }

  /* Bodies only after every system's terrain exists: try_place_body() reads
   * the terrain to decide whether a body belongs there. */
  for (i = 0; i < num_systems; i++) {
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

  /* Homes are picked from the finished systems, so this must follow
   * population: choose_home_systems() weighs each system by what is
   * actually in it. */
  home = fc_malloc(MAX(1, player_count()) * sizeof(*home));
  n_home = choose_home_systems(home);
  if (n_home < player_count() || !place_start_positions(home, n_home)) {
    ok = FALSE;
  }
  free(home);

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

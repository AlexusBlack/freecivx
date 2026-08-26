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

#ifndef FC__SPACE_MAP_H
#define FC__SPACE_MAP_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include "support.h"            /* bool type */

/* Generate a map of star systems. Requires a ruleset defining the space
 * terrains and celestial-body resources; see
 * docs/space_map_generator_plan.md.
 *
 * Returns FALSE if the map could not be generated, in which case the caller
 * must not fall back to another generator - an Earth map on a space ruleset
 * is not a usable game. */
bool map_generate_space(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* FC__SPACE_MAP_H */

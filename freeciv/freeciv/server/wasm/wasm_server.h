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
#ifndef FC__WASM_SERVER_H
#define FC__WASM_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#ifdef __EMSCRIPTEN__

#include <emscripten.h>

/* Server lifecycle states for WASM event loop */
typedef enum {
  WASM_SERVER_NOT_STARTED = 0,
  WASM_SERVER_INITIALIZING,
  WASM_SERVER_WAITING_FOR_PLAYERS,  /* S_S_INITIAL - pregame lobby */
  WASM_SERVER_STARTING_GAME,
  WASM_SERVER_RUNNING_TURN,         /* S_S_RUNNING - in game */
  WASM_SERVER_BETWEEN_TURNS,
  WASM_SERVER_GAME_OVER,            /* S_S_OVER */
  WASM_SERVER_SHUTTING_DOWN
} wasm_server_state;

/* Initialize the WASM server - call once at startup */
EMSCRIPTEN_KEEPALIVE
void wasm_server_init(void);

/* Main tick function - call from JavaScript's requestAnimationFrame or setInterval
 * Returns current server state */
EMSCRIPTEN_KEEPALIVE
wasm_server_state wasm_server_tick(void);

/* Get current server state */
EMSCRIPTEN_KEEPALIVE
wasm_server_state wasm_server_get_state(void);

/* Graceful shutdown */
EMSCRIPTEN_KEEPALIVE
void wasm_server_shutdown(void);

/* Network callbacks from JavaScript */

/* Accept a new connection from JavaScript
 * Returns connection ID (>= 0) on success, -1 on failure */
EMSCRIPTEN_KEEPALIVE
int wasm_net_accept_connection(const char *client_addr);

/* Receive data for a connection
 * conn_id: connection ID returned from wasm_net_accept_connection
 * data: pointer to received bytes
 * len: number of bytes
 * Returns 0 on success, -1 on error */
EMSCRIPTEN_KEEPALIVE
int wasm_net_receive_data(int conn_id, const unsigned char *data, int len);

/* Notify that a connection has been closed by the client */
EMSCRIPTEN_KEEPALIVE
void wasm_net_connection_closed(int conn_id);

/* Console command input (replaces stdin) */
EMSCRIPTEN_KEEPALIVE
void wasm_console_input(const char *command);

#endif /* __EMSCRIPTEN__ */

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* FC__WASM_SERVER_H */

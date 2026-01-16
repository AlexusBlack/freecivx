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
#ifndef FC__WASM_NET_H
#define FC__WASM_NET_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#ifdef __EMSCRIPTEN__

/* For bool type */
#include "support.h"

struct connection;

/* Maximum number of WASM connections */
#define WASM_MAX_CONNECTIONS 128

/* Initialize WASM networking */
void wasm_net_init(void);

/* Shutdown WASM networking */
void wasm_net_shutdown(void);

/* Process all pending network data */
void wasm_net_process_pending(void);

/* Get the connection struct for a WASM connection ID */
struct connection *wasm_net_get_connection(int conn_id);

/* Send data to a WASM connection (called from Freeciv internals) */
int wasm_net_send(int conn_id, const unsigned char *data, int len);

/* Get WASM connection ID from a Freeciv connection */
int wasm_net_get_conn_id(struct connection *pconn);

/* Custom write function for WASM sockets */
int wasm_write_socket(int sock, const unsigned char *data, int len);

/* Check if a socket descriptor is a WASM connection */
bool wasm_is_wasm_socket(int sock);

#endif /* __EMSCRIPTEN__ */

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* FC__WASM_NET_H */

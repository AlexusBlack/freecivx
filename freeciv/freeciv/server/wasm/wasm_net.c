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

#ifdef __EMSCRIPTEN__

#ifdef HAVE_CONFIG_H
#include <fc_config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <emscripten.h>

/* utility */
#include "log.h"
#include "mem.h"
#include "support.h"

/* common */
#include "packets.h"

/* common/networking */
#include "connection.h"

/* server */
#include "connecthand.h"
#include "sernet.h"
#include "srv_main.h"

#include "wasm_net.h"
#include "wasm_server.h"

/**********************************************************************//**
  Connection state for WASM
**************************************************************************/
typedef struct {
  bool in_use;
  int freeciv_conn_id;          /* ID in Freeciv's connection system */
  struct connection *pconn;     /* Pointer to Freeciv connection struct */
  bool has_pending_data;        /* New data received, needs processing */
} wasm_connection_t;

static wasm_connection_t wasm_connections[WASM_MAX_CONNECTIONS];
static bool wasm_net_initialized = FALSE;

/* JavaScript callbacks */
EM_JS(void, js_net_send_data, (int conn_id, const unsigned char *data, int len), {
  if (Module.onServerSendData) {
    const dataArray = new Uint8Array(HEAPU8.buffer, data, len);
    Module.onServerSendData(conn_id, new Uint8Array(dataArray));
  }
});

EM_JS(void, js_net_log, (const char *msg), {
  console.log('[FreecivNet] ' + UTF8ToString(msg));
});

/**********************************************************************//**
  Process incoming packets for a connection using public APIs
  This replaces the static incoming_client_packets() from sernet.c
**************************************************************************/
static void wasm_process_client_packets(struct connection *pconn)
{
  void *packet_data;
  enum packet_type packet_type;

  /* Keep processing packets while there are complete ones in the buffer */
  while ((packet_data = get_packet_from_connection(pconn, &packet_type)) != NULL) {
    bool command_ok;

    pconn->server.last_request_id_seen++;

    /* Process the packet using the public server_packet_input function */
    command_ok = server_packet_input(pconn, packet_data, packet_type);

    /* Free the packet data */
    free(packet_data);

    if (!command_ok) {
      connection_close_server(pconn, _("rejected"));
      break;
    }
  }
}

/**********************************************************************//**
  Initialize WASM networking
**************************************************************************/
void wasm_net_init(void)
{
  int i;

  for (i = 0; i < WASM_MAX_CONNECTIONS; i++) {
    wasm_connections[i].in_use = FALSE;
    wasm_connections[i].freeciv_conn_id = -1;
    wasm_connections[i].pconn = NULL;
    wasm_connections[i].has_pending_data = FALSE;
  }

  wasm_net_initialized = TRUE;
  js_net_log("WASM networking initialized");
}

/**********************************************************************//**
  Shutdown WASM networking
**************************************************************************/
void wasm_net_shutdown(void)
{
  int i;

  for (i = 0; i < WASM_MAX_CONNECTIONS; i++) {
    if (wasm_connections[i].in_use && wasm_connections[i].pconn != NULL) {
      connection_close_server(wasm_connections[i].pconn, _("server shutdown"));
    }
    wasm_connections[i].in_use = FALSE;
    wasm_connections[i].pconn = NULL;
  }

  wasm_net_initialized = FALSE;
  js_net_log("WASM networking shutdown");
}

/**********************************************************************//**
  Find a free WASM connection slot
**************************************************************************/
static int find_free_wasm_slot(void)
{
  int i;

  for (i = 0; i < WASM_MAX_CONNECTIONS; i++) {
    if (!wasm_connections[i].in_use) {
      return i;
    }
  }
  return -1;
}

/**********************************************************************//**
  Get connection struct for a WASM connection ID
**************************************************************************/
struct connection *wasm_net_get_connection(int conn_id)
{
  if (conn_id < 0 || conn_id >= WASM_MAX_CONNECTIONS) {
    return NULL;
  }
  if (!wasm_connections[conn_id].in_use) {
    return NULL;
  }
  return wasm_connections[conn_id].pconn;
}

/**********************************************************************//**
  Accept a new connection from JavaScript
  This creates a Freeciv connection struct and associates it with a WASM ID
**************************************************************************/
EMSCRIPTEN_KEEPALIVE
int wasm_net_accept_connection(const char *client_addr)
{
  int wasm_id;
  int fc_result;
  char addr_copy[256];

  if (!wasm_net_initialized) {
    js_net_log("ERROR: WASM net not initialized");
    return -1;
  }

  /* Find a free WASM connection slot */
  wasm_id = find_free_wasm_slot();
  if (wasm_id < 0) {
    js_net_log("ERROR: No free WASM connection slots");
    return -1;
  }

  /* Copy address */
  if (client_addr != NULL) {
    strncpy(addr_copy, client_addr, sizeof(addr_copy) - 1);
    addr_copy[sizeof(addr_copy) - 1] = '\0';
  } else {
    strcpy(addr_copy, "wasm-client");
  }

  /* Create a Freeciv connection
   * We use a fake socket descriptor (negative to distinguish from real sockets)
   * The sock field will be -(wasm_id + 1) to ensure it's always negative and unique */
  fc_result = server_make_connection(-(wasm_id + 1), addr_copy, addr_copy);

  if (fc_result < 0) {
    js_net_log("ERROR: Failed to create Freeciv connection");
    return -1;
  }

  /* Find the connection that was just created by looking for our fake socket */
  conn_list_iterate(game.all_connections, pconn) {
    if (pconn->sock == -(wasm_id + 1)) {
      wasm_connections[wasm_id].in_use = TRUE;
      wasm_connections[wasm_id].freeciv_conn_id = pconn->id;
      wasm_connections[wasm_id].pconn = pconn;
      wasm_connections[wasm_id].has_pending_data = FALSE;

      char msg[256];
      snprintf(msg, sizeof(msg), "New connection accepted: wasm_id=%d, fc_id=%d, addr=%s",
               wasm_id, pconn->id, addr_copy);
      js_net_log(msg);

      return wasm_id;
    }
  } conn_list_iterate_end;

  js_net_log("ERROR: Could not find newly created connection");
  return -1;
}

/**********************************************************************//**
  Receive data for a connection from JavaScript
**************************************************************************/
EMSCRIPTEN_KEEPALIVE
int wasm_net_receive_data(int conn_id, const unsigned char *data, int len)
{
  struct connection *pconn;
  struct socket_packet_buffer *buffer;

  if (conn_id < 0 || conn_id >= WASM_MAX_CONNECTIONS) {
    js_net_log("ERROR: Invalid connection ID in receive_data");
    return -1;
  }

  if (!wasm_connections[conn_id].in_use) {
    js_net_log("ERROR: Connection not in use");
    return -1;
  }

  pconn = wasm_connections[conn_id].pconn;
  if (pconn == NULL || !pconn->used) {
    js_net_log("ERROR: Freeciv connection not valid");
    return -1;
  }

  buffer = pconn->buffer;
  if (buffer == NULL) {
    js_net_log("ERROR: Connection buffer is NULL");
    return -1;
  }

  /* Ensure buffer has enough space */
  if (buffer->ndata + len > buffer->nsize) {
    int new_size = buffer->ndata + len + 4096;
    unsigned char *new_data = fc_realloc(buffer->data, new_size);
    if (new_data == NULL) {
      js_net_log("ERROR: Failed to grow buffer");
      return -1;
    }
    buffer->data = new_data;
    buffer->nsize = new_size;
  }

  /* Copy data into buffer */
  memcpy(buffer->data + buffer->ndata, data, len);
  buffer->ndata += len;

  /* Mark connection as having pending data */
  wasm_connections[conn_id].has_pending_data = TRUE;

  return 0;
}

/**********************************************************************//**
  Notify that a connection was closed by the client
**************************************************************************/
EMSCRIPTEN_KEEPALIVE
void wasm_net_connection_closed(int conn_id)
{
  struct connection *pconn;

  if (conn_id < 0 || conn_id >= WASM_MAX_CONNECTIONS) {
    return;
  }

  if (!wasm_connections[conn_id].in_use) {
    return;
  }

  pconn = wasm_connections[conn_id].pconn;
  if (pconn != NULL && pconn->used) {
    connection_close_server(pconn, _("client disconnected"));
  }

  wasm_connections[conn_id].in_use = FALSE;
  wasm_connections[conn_id].pconn = NULL;
  wasm_connections[conn_id].freeciv_conn_id = -1;
  wasm_connections[conn_id].has_pending_data = FALSE;

  char msg[128];
  snprintf(msg, sizeof(msg), "Connection closed: wasm_id=%d", conn_id);
  js_net_log(msg);
}

/**********************************************************************//**
  Process all pending network data
  Called from wasm_server_tick()
**************************************************************************/
void wasm_net_process_pending(void)
{
  int i;

  /* Process incoming data for all connections */
  for (i = 0; i < WASM_MAX_CONNECTIONS; i++) {
    if (wasm_connections[i].in_use
        && wasm_connections[i].has_pending_data
        && wasm_connections[i].pconn != NULL) {
      struct connection *pconn = wasm_connections[i].pconn;

      if (pconn->used && !pconn->server.is_closing) {
        /* Process incoming packets using public APIs */
        wasm_process_client_packets(pconn);
      }

      wasm_connections[i].has_pending_data = FALSE;
    }
  }

  /* Flush all send buffers */
  for (i = 0; i < WASM_MAX_CONNECTIONS; i++) {
    if (wasm_connections[i].in_use && wasm_connections[i].pconn != NULL) {
      struct connection *pconn = wasm_connections[i].pconn;

      if (pconn->used
          && !pconn->server.is_closing
          && pconn->send_buffer != NULL
          && pconn->send_buffer->ndata > 0) {
        /* Send the data via JavaScript */
        js_net_send_data(i, pconn->send_buffer->data, pconn->send_buffer->ndata);

        /* Clear the send buffer */
        pconn->send_buffer->ndata = 0;
      }
    }
  }

  /* Check for connections that were closed during processing */
  for (i = 0; i < WASM_MAX_CONNECTIONS; i++) {
    if (wasm_connections[i].in_use && wasm_connections[i].pconn != NULL) {
      struct connection *pconn = wasm_connections[i].pconn;

      if (!pconn->used || pconn->server.is_closing) {
        /* Connection was closed, clean up our state */
        wasm_connections[i].in_use = FALSE;
        wasm_connections[i].pconn = NULL;
        wasm_connections[i].freeciv_conn_id = -1;
        wasm_connections[i].has_pending_data = FALSE;
      }
    }
  }
}

/**********************************************************************//**
  Send data to a WASM connection
  This is called when Freeciv wants to send data to a client
**************************************************************************/
int wasm_net_send(int conn_id, const unsigned char *data, int len)
{
  if (conn_id < 0 || conn_id >= WASM_MAX_CONNECTIONS) {
    return -1;
  }

  if (!wasm_connections[conn_id].in_use) {
    return -1;
  }

  /* Send directly via JavaScript */
  js_net_send_data(conn_id, data, len);

  return len;
}

/**********************************************************************//**
  Get WASM connection ID from a Freeciv connection
  Returns -1 if not a WASM connection
**************************************************************************/
int wasm_net_get_conn_id(struct connection *pconn)
{
  int i;

  if (pconn == NULL) {
    return -1;
  }

  /* Check if this is a WASM connection by looking at the socket fd */
  if (pconn->sock >= 0) {
    /* Real socket, not a WASM connection */
    return -1;
  }

  /* sock is -(wasm_id + 1), so wasm_id = -(sock + 1) */
  int wasm_id = -(pconn->sock + 1);

  if (wasm_id >= 0 && wasm_id < WASM_MAX_CONNECTIONS) {
    if (wasm_connections[wasm_id].in_use
        && wasm_connections[wasm_id].pconn == pconn) {
      return wasm_id;
    }
  }

  /* Search for it */
  for (i = 0; i < WASM_MAX_CONNECTIONS; i++) {
    if (wasm_connections[i].in_use && wasm_connections[i].pconn == pconn) {
      return i;
    }
  }

  return -1;
}

/**********************************************************************//**
  Custom write function for WASM connections
  This can be used to hook into the network layer
**************************************************************************/
int wasm_write_socket(int sock, const unsigned char *data, int len)
{
  /* sock is -(wasm_id + 1), so wasm_id = -(sock + 1) */
  int wasm_id = -(sock + 1);

  if (wasm_id < 0 || wasm_id >= WASM_MAX_CONNECTIONS) {
    return -1;
  }

  return wasm_net_send(wasm_id, data, len);
}

/**********************************************************************//**
  Check if a socket descriptor is a WASM connection
**************************************************************************/
bool wasm_is_wasm_socket(int sock)
{
  return sock < 0;
}

#endif /* __EMSCRIPTEN__ */

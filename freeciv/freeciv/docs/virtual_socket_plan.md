# Virtual Socket Implementation Plan for Emscripten

## Overview

This document outlines the plan to replace native sockets with an in-memory emulation for Emscripten environments, enabling the Freeciv server to run in a web browser with JavaScript webclients connecting via virtual sockets.

---

## Analysis of `utility/netintf.c`

### Current Socket Abstraction Functions

| Function | Purpose | Lines |
|----------|---------|-------|
| `fc_connect()` | Connect socket to address | 107-120 |
| `fc_select()` | Wait for socket activity | 125-139 |
| `fc_readsocket()` | Read from socket | 144-158 |
| `fc_writesocket()` | Write to socket | 163-181 |
| `fc_closesocket()` | Close socket | 186-193 |
| `fc_nonblock()` | Set non-blocking mode | 227-264 |
| `fc_init_network()` | Initialize networking | 198-212 |
| `fc_shutdown_network()` | Shutdown networking | 217-222 |

### Server Raw Socket Calls in `server/sernet.c`

| Function | Purpose | Line |
|----------|---------|------|
| `socket()` | Create sockets | 1181, 1285 |
| `bind()` | Bind to address | 1216, 1322 |
| `listen()` | Listen for connections | 1242 |
| `accept()` | Accept connections | 1008 |

---

## Proposed Architecture

### Strategy: Conditional Compilation with Separate Implementation File

- **New file:** `utility/netintf_virtual.c` - Complete virtual socket implementation for Emscripten
- **New file:** `utility/netintf_virtual.h` - Header with structures and JS API
- **Minimal changes** to `netintf.c` - Just a preprocessor wrapper

---

## New Files

### 1. `utility/netintf_virtual.h`

```c
#ifndef FC__NETINTF_VIRTUAL_H
#define FC__NETINTF_VIRTUAL_H

#ifdef __EMSCRIPTEN__

#include <emscripten.h>
#include <stddef.h>
#include <stdbool.h>

/* Virtual socket descriptor range (avoid conflicts with real fds) */
#define VSOCK_BASE_FD      1000
#define VSOCK_MAX_SOCKETS  256

/* Virtual socket states */
enum vsock_state {
  VSOCK_CLOSED = 0,
  VSOCK_LISTENING,
  VSOCK_CONNECTED,
  VSOCK_CONNECTING
};

/* Ring buffer for socket data */
struct vsock_buffer {
  unsigned char *data;
  size_t capacity;
  size_t head;      /* read position */
  size_t tail;      /* write position */
  size_t size;      /* current data size */
};

/* Virtual socket structure */
struct virtual_socket {
  int fd;                       /* virtual file descriptor */
  enum vsock_state state;
  bool nonblocking;

  struct vsock_buffer recv_buf; /* incoming data from JS */
  struct vsock_buffer send_buf; /* outgoing data to JS */

  /* For listening sockets: pending connections queue */
  int *pending_connections;
  int pending_count;
  int pending_capacity;

  /* For connected sockets: peer reference */
  int peer_fd;                  /* connected peer's fd */
  int client_id;                /* JS client identifier */
};

/*--- Internal API ---*/
int vsock_alloc(void);
void vsock_free(struct virtual_socket *vs);
struct virtual_socket *vsock_get(int fd);
struct virtual_socket *vsock_find_by_client(int client_id);

/*--- Buffer operations ---*/
void vsock_buffer_init(struct vsock_buffer *buf, size_t capacity);
void vsock_buffer_free(struct vsock_buffer *buf);
int vsock_buffer_write(struct vsock_buffer *buf, const void *data, size_t len);
int vsock_buffer_read(struct vsock_buffer *buf, void *data, size_t max_len);

/*--- Socket replacements for sernet.c ---*/
int vsock_socket(int domain, int type, int protocol);
int vsock_bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int vsock_listen(int sockfd, int backlog);
int vsock_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);

/*--- Exported to JS via Emscripten ---*/
EMSCRIPTEN_KEEPALIVE int vsock_js_connect(int client_id);
EMSCRIPTEN_KEEPALIVE int vsock_js_send(int client_id, const void *data, size_t len);
EMSCRIPTEN_KEEPALIVE int vsock_js_recv(int client_id, void *buf, size_t max_len);
EMSCRIPTEN_KEEPALIVE int vsock_js_disconnect(int client_id);
EMSCRIPTEN_KEEPALIVE int vsock_js_poll(void);
EMSCRIPTEN_KEEPALIVE int vsock_js_has_data(int client_id);

#endif /* __EMSCRIPTEN__ */

#endif /* FC__NETINTF_VIRTUAL_H */
```

### 2. `utility/netintf_virtual.c`

```c
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

/***********************************************************************
  Virtual socket implementation for Emscripten/WebAssembly builds.
  Replaces native sockets with in-memory buffers accessible from JS.
***********************************************************************/

#ifdef __EMSCRIPTEN__

#include <emscripten.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#include "fc_prehdrs.h"
#include "log.h"
#include "mem.h"
#include "support.h"
#include "netintf.h"
#include "netintf_virtual.h"

/*--- Static data ---*/
static struct virtual_socket vsock_table[VSOCK_MAX_SOCKETS];
static int vsock_listen_fd = -1;
static int vsock_next_fd = VSOCK_BASE_FD;
static bool vsock_initialized = false;

/*=======================================================================
  Buffer Operations
=======================================================================*/

void vsock_buffer_init(struct vsock_buffer *buf, size_t capacity)
{
  buf->data = fc_calloc(1, capacity);
  buf->capacity = capacity;
  buf->head = 0;
  buf->tail = 0;
  buf->size = 0;
}

void vsock_buffer_free(struct vsock_buffer *buf)
{
  if (buf->data) {
    free(buf->data);
    buf->data = NULL;
  }
  buf->capacity = 0;
  buf->size = 0;
}

int vsock_buffer_write(struct vsock_buffer *buf, const void *data, size_t len)
{
  if (!buf->data || len == 0) return 0;

  size_t available = buf->capacity - buf->size;
  size_t to_write = (len < available) ? len : available;

  const unsigned char *src = (const unsigned char *)data;

  for (size_t i = 0; i < to_write; i++) {
    buf->data[buf->tail] = src[i];
    buf->tail = (buf->tail + 1) % buf->capacity;
  }
  buf->size += to_write;

  return (int)to_write;
}

int vsock_buffer_read(struct vsock_buffer *buf, void *data, size_t max_len)
{
  if (!buf->data || buf->size == 0) {
    errno = EWOULDBLOCK;
    return -1;
  }

  size_t to_read = (max_len < buf->size) ? max_len : buf->size;
  unsigned char *dst = (unsigned char *)data;

  for (size_t i = 0; i < to_read; i++) {
    dst[i] = buf->data[buf->head];
    buf->head = (buf->head + 1) % buf->capacity;
  }
  buf->size -= to_read;

  return (int)to_read;
}

/*=======================================================================
  Virtual Socket Management
=======================================================================*/

static void vsock_init_table(void)
{
  if (!vsock_initialized) {
    memset(vsock_table, 0, sizeof(vsock_table));
    vsock_initialized = true;
  }
}

int vsock_alloc(void)
{
  vsock_init_table();

  for (int i = 0; i < VSOCK_MAX_SOCKETS; i++) {
    if (vsock_table[i].state == VSOCK_CLOSED) {
      vsock_table[i].fd = vsock_next_fd++;
      vsock_table[i].state = VSOCK_CONNECTING; /* mark as in-use */
      vsock_table[i].nonblocking = false;
      vsock_table[i].client_id = -1;
      return vsock_table[i].fd;
    }
  }

  errno = EMFILE;
  return -1;
}

void vsock_free(struct virtual_socket *vs)
{
  if (!vs) return;

  vsock_buffer_free(&vs->recv_buf);
  vsock_buffer_free(&vs->send_buf);

  if (vs->pending_connections) {
    free(vs->pending_connections);
    vs->pending_connections = NULL;
  }

  memset(vs, 0, sizeof(*vs));
}

struct virtual_socket *vsock_get(int fd)
{
  if (fd < VSOCK_BASE_FD) return NULL;

  for (int i = 0; i < VSOCK_MAX_SOCKETS; i++) {
    if (vsock_table[i].fd == fd && vsock_table[i].state != VSOCK_CLOSED) {
      return &vsock_table[i];
    }
  }
  return NULL;
}

struct virtual_socket *vsock_find_by_client(int client_id)
{
  for (int i = 0; i < VSOCK_MAX_SOCKETS; i++) {
    if (vsock_table[i].client_id == client_id
        && vsock_table[i].state == VSOCK_CONNECTED) {
      return &vsock_table[i];
    }
  }
  return NULL;
}

/*=======================================================================
  Socket API Replacements (for sernet.c)
=======================================================================*/

int vsock_socket(int domain, int type, int protocol)
{
  return vsock_alloc();
}

int vsock_bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
  struct virtual_socket *vs = vsock_get(sockfd);
  if (!vs) {
    errno = EBADF;
    return -1;
  }
  /* Virtual bind - just mark as ready */
  return 0;
}

int vsock_listen(int sockfd, int backlog)
{
  struct virtual_socket *vs = vsock_get(sockfd);
  if (!vs) {
    errno = EBADF;
    return -1;
  }

  vs->state = VSOCK_LISTENING;
  vs->pending_capacity = (backlog > 0) ? backlog : 16;
  vs->pending_connections = fc_calloc(vs->pending_capacity, sizeof(int));
  vs->pending_count = 0;

  vsock_listen_fd = sockfd;

  log_verbose("Virtual socket %d listening (backlog=%d)", sockfd, backlog);
  return 0;
}

int vsock_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
  struct virtual_socket *listen_vs = vsock_get(sockfd);
  if (!listen_vs || listen_vs->state != VSOCK_LISTENING) {
    errno = EINVAL;
    return -1;
  }

  if (listen_vs->pending_count == 0) {
    errno = EWOULDBLOCK;
    return -1;
  }

  /* Pop first pending connection */
  int client_fd = listen_vs->pending_connections[0];

  /* Shift remaining */
  for (int i = 1; i < listen_vs->pending_count; i++) {
    listen_vs->pending_connections[i - 1] = listen_vs->pending_connections[i];
  }
  listen_vs->pending_count--;

  /* Fill in dummy address if requested */
  if (addr && addrlen) {
    struct sockaddr_in *sin = (struct sockaddr_in *)addr;
    memset(sin, 0, sizeof(*sin));
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = htonl(0x7F000001); /* 127.0.0.1 */
    *addrlen = sizeof(*sin);
  }

  log_verbose("Virtual accept: new connection fd=%d", client_fd);
  return client_fd;
}

/*=======================================================================
  fc_* Network Interface Replacements
=======================================================================*/

int fc_connect(int sockfd, const struct sockaddr *serv_addr, socklen_t addrlen)
{
  /* Server doesn't use connect - it accepts connections */
  /* For future client support, this would queue a connection request */
  errno = ENOSYS;
  return -1;
}

int fc_select(int n, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
              fc_timeval *timeout)
{
  int ready = 0;
  fd_set new_readfds, new_writefds, new_exceptfds;

  FC_FD_ZERO(&new_readfds);
  FC_FD_ZERO(&new_writefds);
  FC_FD_ZERO(&new_exceptfds);

  /* Check each virtual socket */
  for (int i = 0; i < VSOCK_MAX_SOCKETS; i++) {
    struct virtual_socket *vs = &vsock_table[i];
    if (vs->state == VSOCK_CLOSED) continue;

    int fd = vs->fd;

    /* Check read readiness */
    if (readfds && FD_ISSET(fd, readfds)) {
      bool readable = false;

      if (vs->state == VSOCK_LISTENING && vs->pending_count > 0) {
        readable = true;
      } else if (vs->state == VSOCK_CONNECTED && vs->recv_buf.size > 0) {
        readable = true;
      }

      if (readable) {
        FD_SET(fd, &new_readfds);
        ready++;
      }
    }

    /* Check write readiness */
    if (writefds && FD_ISSET(fd, writefds)) {
      if (vs->state == VSOCK_CONNECTED) {
        /* Always writable (we buffer internally) */
        FD_SET(fd, &new_writefds);
        ready++;
      }
    }

    /* Exceptfds - not used for virtual sockets */
  }

  if (readfds) *readfds = new_readfds;
  if (writefds) *writefds = new_writefds;
  if (exceptfds) FC_FD_ZERO(exceptfds);

  return ready;
}

int fc_readsocket(int sock, void *buf, size_t size)
{
  struct virtual_socket *vs = vsock_get(sock);
  if (!vs) {
    errno = EBADF;
    return -1;
  }

  if (vs->state != VSOCK_CONNECTED) {
    errno = ENOTCONN;
    return -1;
  }

  if (vs->recv_buf.size == 0) {
    if (vs->nonblocking) {
      errno = EWOULDBLOCK;
      return -1;
    }
    return 0; /* No data available */
  }

  return vsock_buffer_read(&vs->recv_buf, buf, size);
}

int fc_writesocket(int sock, const void *buf, size_t size)
{
  struct virtual_socket *vs = vsock_get(sock);
  if (!vs) {
    errno = EBADF;
    return -1;
  }

  if (vs->state != VSOCK_CONNECTED) {
    errno = ENOTCONN;
    return -1;
  }

  int written = vsock_buffer_write(&vs->send_buf, buf, size);

  /* Notify JS that data is available */
  EM_ASM({
    if (typeof Module !== 'undefined' && Module.onServerData) {
      Module.onServerData($0);
    }
  }, vs->client_id);

  return written;
}

void fc_closesocket(int sock)
{
  struct virtual_socket *vs = vsock_get(sock);
  if (!vs) return;

  int client_id = vs->client_id;

  /* Notify JS of disconnection */
  if (client_id >= 0) {
    EM_ASM({
      if (typeof Module !== 'undefined' && Module.onServerDisconnect) {
        Module.onServerDisconnect($0);
      }
    }, client_id);
  }

  log_verbose("Virtual socket %d closed (client_id=%d)", sock, client_id);
  vsock_free(vs);
}

void fc_init_network(void)
{
  vsock_init_table();
  log_verbose("Virtual network initialized for Emscripten");
}

void fc_shutdown_network(void)
{
  /* Clean up all sockets */
  for (int i = 0; i < VSOCK_MAX_SOCKETS; i++) {
    if (vsock_table[i].state != VSOCK_CLOSED) {
      vsock_free(&vsock_table[i]);
    }
  }
  vsock_initialized = false;
  log_verbose("Virtual network shutdown");
}

void fc_nonblock(int sockfd)
{
  struct virtual_socket *vs = vsock_get(sockfd);
  if (vs) {
    vs->nonblocking = true;
  }
}

/*=======================================================================
  JavaScript Interface (Exported via EMSCRIPTEN_KEEPALIVE)
=======================================================================*/

EMSCRIPTEN_KEEPALIVE
int vsock_js_connect(int client_id)
{
  struct virtual_socket *listen_vs = vsock_get(vsock_listen_fd);
  if (!listen_vs || listen_vs->state != VSOCK_LISTENING) {
    log_error("vsock_js_connect: no listening socket");
    return -1;
  }

  /* Create new socket for this connection */
  int new_fd = vsock_alloc();
  if (new_fd < 0) {
    log_error("vsock_js_connect: failed to allocate socket");
    return -1;
  }

  struct virtual_socket *vs = vsock_get(new_fd);
  vs->state = VSOCK_CONNECTED;
  vs->client_id = client_id;
  vs->nonblocking = true;
  vsock_buffer_init(&vs->recv_buf, 65536);
  vsock_buffer_init(&vs->send_buf, 65536);

  /* Queue in listen socket's pending list */
  if (listen_vs->pending_count < listen_vs->pending_capacity) {
    listen_vs->pending_connections[listen_vs->pending_count++] = new_fd;
    log_verbose("vsock_js_connect: client %d queued as fd %d", client_id, new_fd);
    return new_fd;
  }

  /* Queue full - reject */
  vsock_free(vs);
  log_error("vsock_js_connect: connection queue full");
  return -1;
}

EMSCRIPTEN_KEEPALIVE
int vsock_js_send(int client_id, const void *data, size_t len)
{
  struct virtual_socket *vs = vsock_find_by_client(client_id);
  if (!vs) {
    log_error("vsock_js_send: unknown client %d", client_id);
    return -1;
  }

  /* Write to recv_buf - this is data FROM client TO server */
  return vsock_buffer_write(&vs->recv_buf, data, len);
}

EMSCRIPTEN_KEEPALIVE
int vsock_js_recv(int client_id, void *buf, size_t max_len)
{
  struct virtual_socket *vs = vsock_find_by_client(client_id);
  if (!vs) {
    return -1;
  }

  if (vs->send_buf.size == 0) {
    return 0; /* No data */
  }

  /* Read from send_buf - this is data FROM server TO client */
  return vsock_buffer_read(&vs->send_buf, buf, max_len);
}

EMSCRIPTEN_KEEPALIVE
int vsock_js_disconnect(int client_id)
{
  struct virtual_socket *vs = vsock_find_by_client(client_id);
  if (vs) {
    log_verbose("vsock_js_disconnect: client %d", client_id);
    /* Mark as closed - server will detect on next operation */
    vs->recv_buf.size = 0; /* Signal EOF */
    vs->state = VSOCK_CLOSED;
  }
  return 0;
}

EMSCRIPTEN_KEEPALIVE
int vsock_js_poll(void)
{
  /* Return count of sockets with pending data */
  int count = 0;
  for (int i = 0; i < VSOCK_MAX_SOCKETS; i++) {
    if (vsock_table[i].state == VSOCK_CONNECTED
        && vsock_table[i].send_buf.size > 0) {
      count++;
    }
  }
  return count;
}

EMSCRIPTEN_KEEPALIVE
int vsock_js_has_data(int client_id)
{
  struct virtual_socket *vs = vsock_find_by_client(client_id);
  if (!vs) return 0;
  return (vs->send_buf.size > 0) ? 1 : 0;
}

#endif /* __EMSCRIPTEN__ */
```

---

## Changes to Existing Files

### 1. `utility/netintf.c` - Minimal Wrapper

Add at the **very top** of the file, before all includes:

```c
#ifdef __EMSCRIPTEN__
/* For Emscripten builds, use virtual socket implementation */
#include "netintf_virtual.c"
#else
```

Add at the **very end** of the file:

```c
#endif /* !__EMSCRIPTEN__ */
```

**Total change: 4 lines**

### 2. `server/sernet.c` - Socket Macro Redirects

Add after the existing includes (around line 90):

```c
#ifdef __EMSCRIPTEN__
#include "utility/netintf_virtual.h"
#define socket(d,t,p)     vsock_socket(d,t,p)
#define bind(s,a,l)       vsock_bind(s,a,l)
#define listen(s,b)       vsock_listen(s,b)
#define accept(s,a,l)     vsock_accept(s,a,l)
#endif
```

**Total change: 7 lines**

### 3. Build System

#### For Autotools (`utility/Makefile.am`):

```makefile
if EMSCRIPTEN
# Virtual socket implementation includes itself
libfcutility_la_SOURCES += netintf_virtual.h
else
# Standard build
endif
```

#### For Meson (`utility/meson.build`):

```meson
if get_option('emscripten')
  utility_sources += files('netintf_virtual.c', 'netintf_virtual.h')
endif
```

---

## JavaScript Client Integration

### Example Usage

```javascript
// FreecivServer.js - JavaScript interface to virtual sockets

const FreecivServer = {
  clientId: 0,
  pollInterval: null,

  /**
   * Connect to the in-browser Freeciv server
   * @returns {number} File descriptor or -1 on error
   */
  connect() {
    this.clientId = Date.now() & 0xFFFFFF; // Unique client ID
    const fd = Module._vsock_js_connect(this.clientId);

    if (fd >= 0) {
      console.log(`Connected to Freeciv server, fd=${fd}, clientId=${this.clientId}`);
      this.startPolling();
    }

    return fd;
  },

  /**
   * Send data to the server
   * @param {Uint8Array} data - Binary data to send
   * @returns {number} Bytes sent or -1 on error
   */
  send(data) {
    const buf = Module._malloc(data.length);
    Module.HEAPU8.set(data, buf);
    const sent = Module._vsock_js_send(this.clientId, buf, data.length);
    Module._free(buf);
    return sent;
  },

  /**
   * Receive data from the server
   * @param {number} maxLen - Maximum bytes to receive
   * @returns {Uint8Array} Received data (may be empty)
   */
  recv(maxLen = 65536) {
    const buf = Module._malloc(maxLen);
    const received = Module._vsock_js_recv(this.clientId, buf, maxLen);

    let result = new Uint8Array(0);
    if (received > 0) {
      result = new Uint8Array(Module.HEAPU8.buffer.slice(buf, buf + received));
    }

    Module._free(buf);
    return result;
  },

  /**
   * Check if server has data ready
   * @returns {boolean}
   */
  hasData() {
    return Module._vsock_js_has_data(this.clientId) > 0;
  },

  /**
   * Disconnect from server
   */
  disconnect() {
    this.stopPolling();
    Module._vsock_js_disconnect(this.clientId);
    console.log('Disconnected from Freeciv server');
  },

  /**
   * Start polling for server data
   */
  startPolling() {
    this.pollInterval = setInterval(() => {
      if (this.hasData()) {
        const data = this.recv();
        if (data.length > 0 && this.onData) {
          this.onData(data);
        }
      }
    }, 10); // Poll every 10ms
  },

  /**
   * Stop polling
   */
  stopPolling() {
    if (this.pollInterval) {
      clearInterval(this.pollInterval);
      this.pollInterval = null;
    }
  },

  /**
   * Callback for received data - override this
   * @param {Uint8Array} data
   */
  onData: null
};

// Server-side callbacks (called from C via EM_ASM)
Module.onServerData = (clientId) => {
  // Data available - will be picked up by polling
  // Or trigger immediate receive if needed
};

Module.onServerDisconnect = (clientId) => {
  console.log(`Server closed connection for client ${clientId}`);
  FreecivServer.stopPolling();
  if (FreecivServer.onDisconnect) {
    FreecivServer.onDisconnect();
  }
};

// Export for use
window.FreecivServer = FreecivServer;
```

### Integration with Existing Webclient

```javascript
// In webclient packet handler:
FreecivServer.onData = (data) => {
  // Parse Freeciv packet from binary data
  const packet = parseFreecivPacket(data);
  handleServerPacket(packet);
};

FreecivServer.onDisconnect = () => {
  showDisconnectedDialog();
};

// Connect when page loads
function initGame() {
  const fd = FreecivServer.connect();
  if (fd < 0) {
    showError('Failed to connect to server');
    return;
  }

  // Send initial connection packet
  const helloPacket = buildConnectionPacket(username);
  FreecivServer.send(helloPacket);
}
```

---

## File Summary

### New Files to Create

| File | Purpose | Size (est.) |
|------|---------|-------------|
| `utility/netintf_virtual.h` | Header with structures and JS API declarations | ~80 lines |
| `utility/netintf_virtual.c` | Complete virtual socket implementation | ~400 lines |

### Existing Files to Modify

| File | Change | Lines Changed |
|------|--------|---------------|
| `utility/netintf.c` | Add `#ifdef __EMSCRIPTEN__` wrapper | 4 lines |
| `server/sernet.c` | Add socket macro redirects | 7 lines |
| Build system | Add conditional source selection | ~5 lines |

**Total lines changed in existing files: ~16 lines**

---

## Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                      Web Browser                                │
│  ┌─────────────────────┐     ┌─────────────────────────────┐   │
│  │   JavaScript        │     │   WebAssembly (Emscripten)  │   │
│  │   Webclient         │     │   Freeciv Server            │   │
│  │                     │     │                             │   │
│  │  FreecivServer.js   │     │  ┌─────────────────────┐   │   │
│  │  ┌───────────────┐  │     │  │    sernet.c         │   │   │
│  │  │ send(data)    │──┼─────┼──│► vsock_js_send()    │   │   │
│  │  │ recv()        │◄─┼─────┼──│  vsock_js_recv()    │   │   │
│  │  │ connect()     │──┼─────┼──│► vsock_js_connect() │   │   │
│  │  │ disconnect()  │──┼─────┼──│► vsock_js_disconnect│   │   │
│  │  └───────────────┘  │     │  └──────────┬──────────┘   │   │
│  │                     │     │             │              │   │
│  │  onServerData() ◄───┼─────┼─────────────┤ EM_ASM      │   │
│  │  onDisconnect() ◄───┼─────┼─────────────┘              │   │
│  │                     │     │                             │   │
│  └─────────────────────┘     │  ┌─────────────────────┐   │   │
│                              │  │ netintf_virtual.c   │   │   │
│                              │  │ ┌─────────────────┐ │   │   │
│                              │  │ │ virtual_socket  │ │   │   │
│                              │  │ │ ├─ recv_buf     │ │   │   │
│                              │  │ │ ├─ send_buf     │ │   │   │
│                              │  │ │ └─ state        │ │   │   │
│                              │  │ └─────────────────┘ │   │   │
│                              │  └─────────────────────┘   │   │
│                              └─────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

---

## Implementation Notes

### Thread Safety
- Emscripten runs single-threaded by default
- No mutex/locking needed for buffer operations
- If using pthreads, add `emscripten_atomic_*` operations

### Buffer Sizing
- Default 64KB per connection direction
- Adjust `vsock_buffer_init()` capacity if needed for large packets

### Polling vs Events
- Current design uses polling from JS side
- Alternative: Use `emscripten_async_call()` for event-driven model

### Memory Management
- Buffers allocated via `fc_calloc()` (Freeciv's wrapper)
- Properly freed on socket close
- JS must use `Module._malloc()`/`Module._free()` for data transfer

---

## Testing Strategy

1. **Unit test virtual sockets** - Test buffer operations independently
2. **Integration test** - Connect JS client to WASM server
3. **Packet verification** - Ensure Freeciv packets parse correctly
4. **Stress test** - Multiple clients, large data transfers
5. **Disconnect handling** - Clean shutdown and reconnection

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


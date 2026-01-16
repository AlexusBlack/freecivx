# Freeciv WASM Server

This directory contains the WebAssembly/Emscripten port of the Freeciv server.

## Architecture

The WASM server replaces the traditional blocking event loop with an event-driven
architecture suitable for the browser environment:

```
┌─────────────────────────────────────────────────────────────────┐
│                        Web Browser                               │
│  ┌─────────────┐     ┌──────────────────────────────────────┐  │
│  │ Web Client  │◄───►│         JavaScript Bridge             │  │
│  │   (HTML5)   │     │  - freeciv_server.js                  │  │
│  └─────────────┘     │  - WebSocket management               │  │
│                      │  - Message routing                    │  │
│                      └──────────────┬───────────────────────┘  │
│                                     │                           │
│                      ┌──────────────▼───────────────────────┐  │
│                      │       WASM Freeciv Server             │  │
│                      │  - wasm_server.c (event loop)         │  │
│                      │  - wasm_net.c (network abstraction)   │  │
│                      └──────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

## Files

- `wasm_server.h/c` - Main server lifecycle and tick function
- `wasm_net.h/c` - Network abstraction layer (replaces sockets with JS callbacks)
- `freeciv_server.js` - JavaScript bridge for WebSocket handling
- `example.html` - Demo page showing integration

## Building with Meson

### Prerequisites

1. Emscripten SDK (emsdk) installed and activated
2. Meson build system
3. Ninja build tool

### Quick Start

```bash
# 1. Setup Emscripten (if not already done)
cd /path/to/freeciv
./platforms/emscripten/emssetup.sh /path/to/emsdk

# 2. Activate Emscripten
source /path/to/emsdk/emsdk_env.sh

# 3. Create cross-compile file (or use the template)
cp platforms/emscripten/setups/cross-ems.tmpl cross-ems.txt
# Edit cross-ems.txt to set <EMSDK_ROOT> to your emsdk path

# 4. Configure with Meson
meson setup build-wasm \
  --cross-file cross-ems.txt \
  -Dwasm-server=true \
  -Dserver=disabled \
  -Dclients=[] \
  -Dfcmp=[] \
  -Druledit=false \
  -Dnls=false \
  -Djson-protocol=true

# 5. Build
ninja -C build-wasm

# Output: build-wasm/freeciv-server-wasm.js and freeciv-server-wasm.wasm
```

### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `-Dwasm-server=true` | false | Enable WASM server build |
| `-Dwasm-server-debug=true` | false | Include debug symbols and source maps |
| `-Djson-protocol=true` | false | Use JSON protocol (recommended for web) |

### Debug Build

For development with better error messages and source maps:

```bash
meson setup build-wasm-debug \
  --cross-file cross-ems.txt \
  -Dwasm-server=true \
  -Dwasm-server-debug=true \
  -Dserver=disabled \
  -Dclients=[] \
  -Djson-protocol=true

ninja -C build-wasm-debug
```

### Manual Build (Alternative)

If you prefer not to use meson, you can compile manually:

```bash
# Compile WASM server sources
emcc -c server/wasm/wasm_server.c -o wasm_server.o \
  -Iutility -Icommon -Icommon/networking \
  -Iserver -Iserver/wasm -D__EMSCRIPTEN__

emcc -c server/wasm/wasm_net.c -o wasm_net.o \
  -Iutility -Icommon -Icommon/networking \
  -Iserver -Iserver/wasm -D__EMSCRIPTEN__

# Link with rest of freeciv (after building other .o files)
emcc *.o -o freeciv-server-wasm.js \
  -s WASM=1 \
  -s MODULARIZE=1 \
  -s EXPORT_NAME="createFreecivServer" \
  -s EXPORTED_FUNCTIONS='["_main","_wasm_server_init","_wasm_server_tick","_wasm_server_get_state","_wasm_server_shutdown","_wasm_net_accept_connection","_wasm_net_receive_data","_wasm_net_connection_closed","_wasm_console_input","_malloc","_free"]' \
  -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","UTF8ToString","stringToUTF8","HEAPU8"]' \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s NO_EXIT_RUNTIME=1
```

## API Reference

### C API (WASM exports)

```c
// Initialize the server
void wasm_server_init(void);

// Process one tick - call from setInterval or requestAnimationFrame
wasm_server_state wasm_server_tick(void);

// Get current server state
wasm_server_state wasm_server_get_state(void);

// Shutdown the server
void wasm_server_shutdown(void);

// Accept a new client connection (returns connection ID)
int wasm_net_accept_connection(const char *client_addr);

// Receive data from a client
int wasm_net_receive_data(int conn_id, const unsigned char *data, int len);

// Notify that a client disconnected
void wasm_net_connection_closed(int conn_id);

// Send a console command
void wasm_console_input(const char *command);
```

### JavaScript API

```javascript
// Create server instance
const server = new FreecivServer(Module);

// Initialize and start
server.init();
server.start();  // Starts tick loop

// Accept WebSocket connections
ws.onopen = () => {
  const connId = server.acceptConnection(ws, clientAddr);
};

// Event handlers
server.on('stateChange', ({ oldState, newState }) => {
  console.log(`State: ${server.getStateName(newState)}`);
});

server.on('connection', ({ connId, clientAddr }) => {
  console.log(`New connection: ${connId}`);
});

server.on('disconnect', ({ connId }) => {
  console.log(`Disconnected: ${connId}`);
});

// Send console commands
server.sendCommand('/help');

// Shutdown
server.shutdown();
```

### Module Callbacks

The WASM module expects these callbacks to be set on `Module`:

```javascript
Module.onServerSendData = (connId, data) => {
  // Send data to client via WebSocket
};

Module.onConnectionClosed = (connId, reason) => {
  // Handle server-side connection close
};

Module.onServerStateChange = (newState) => {
  // Handle state changes
};
```

## Server States

| State | Value | Description |
|-------|-------|-------------|
| NOT_STARTED | 0 | Server not initialized |
| INITIALIZING | 1 | Initialization in progress |
| WAITING_FOR_PLAYERS | 2 | Pregame lobby (S_S_INITIAL) |
| STARTING_GAME | 3 | Game is starting |
| RUNNING_TURN | 4 | Game in progress (S_S_RUNNING) |
| BETWEEN_TURNS | 5 | Processing end of turn |
| GAME_OVER | 6 | Game finished (S_S_OVER) |
| SHUTTING_DOWN | 7 | Server shutting down |

## Integration Notes

### WebSocket Protocol

The server uses Freeciv's existing packet protocol. When `FREECIV_JSON_CONNECTION`
is defined, packets are JSON-encoded for easier web integration.

### Threading

WASM is single-threaded. All mutex operations are no-ops. The metaserver
communication should be done via async JavaScript fetch instead of the
threaded approach used in native builds.

### File I/O

For saves and rulesets, use Emscripten's virtual filesystem:
- MEMFS for in-memory storage
- IDBFS for IndexedDB persistence
- Or preload files at build time

### Tick Rate

The default tick rate is 100ms (10 ticks/second). Adjust based on needs:
- Lower for more responsive gameplay
- Higher to reduce CPU usage

```javascript
server.tickRate = 50;  // 20 ticks/second
```

## Limitations

- No real socket support (WebSocket only via JS bridge)
- No threading (single-threaded event loop)
- No stdin (use wasm_console_input instead)
- Metaserver requires async JavaScript implementation

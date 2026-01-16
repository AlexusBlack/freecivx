/**
 * Freeciv WASM Server JavaScript Bridge
 *
 * This module provides the JavaScript side of the WASM server integration.
 * It handles:
 * - WebSocket connections from clients
 * - Routing data between WebSockets and the WASM server
 * - Server lifecycle management
 * - Event loop integration
 */

// Server state enum (mirrors wasm_server_state in C)
const ServerState = {
  NOT_STARTED: 0,
  INITIALIZING: 1,
  WAITING_FOR_PLAYERS: 2,
  STARTING_GAME: 3,
  RUNNING_TURN: 4,
  BETWEEN_TURNS: 5,
  GAME_OVER: 6,
  SHUTTING_DOWN: 7
};

const ServerStateNames = [
  'NOT_STARTED',
  'INITIALIZING',
  'WAITING_FOR_PLAYERS',
  'STARTING_GAME',
  'RUNNING_TURN',
  'BETWEEN_TURNS',
  'GAME_OVER',
  'SHUTTING_DOWN'
];

/**
 * FreecivServer - Main class for managing the WASM Freeciv server
 */
class FreecivServer {
  constructor(wasmModule) {
    this.module = wasmModule;
    this.connections = new Map();  // WebSocket -> conn_id
    this.connById = new Map();     // conn_id -> WebSocket
    this.nextConnId = 0;
    this.tickInterval = null;
    this.tickRate = 100;  // ms between ticks (10 ticks per second)
    this.state = ServerState.NOT_STARTED;
    this.eventHandlers = new Map();

    // Set up Module callbacks
    this._setupModuleCallbacks();
  }

  /**
   * Set up callbacks that WASM code will call
   */
  _setupModuleCallbacks() {
    const self = this;

    // Called when server wants to send data to a client
    this.module.onServerSendData = (connId, data) => {
      self._handleServerSend(connId, data);
    };

    // Called when server closes a connection
    this.module.onConnectionClosed = (connId, reason) => {
      self._handleServerCloseConnection(connId, reason);
    };

    // Called when server state changes
    this.module.onServerStateChange = (newState) => {
      self._handleStateChange(newState);
    };
  }

  /**
   * Initialize the server
   */
  init() {
    console.log('[FreecivServer] Initializing...');
    this.module._wasm_server_init();
  }

  /**
   * Start the server tick loop
   */
  start() {
    if (this.tickInterval !== null) {
      console.warn('[FreecivServer] Server already running');
      return;
    }

    console.log('[FreecivServer] Starting tick loop');
    this.tickInterval = setInterval(() => this._tick(), this.tickRate);
  }

  /**
   * Stop the server tick loop
   */
  stop() {
    if (this.tickInterval !== null) {
      clearInterval(this.tickInterval);
      this.tickInterval = null;
    }
    console.log('[FreecivServer] Tick loop stopped');
  }

  /**
   * Shutdown the server
   */
  shutdown() {
    this.stop();
    this.module._wasm_server_shutdown();

    // Close all WebSocket connections
    for (const ws of this.connections.keys()) {
      ws.close(1000, 'Server shutdown');
    }
    this.connections.clear();
    this.connById.clear();
  }

  /**
   * Single tick of the server
   */
  _tick() {
    try {
      const newState = this.module._wasm_server_tick();
      if (newState !== this.state) {
        this._handleStateChange(newState);
      }
    } catch (e) {
      console.error('[FreecivServer] Error in tick:', e);
    }
  }

  /**
   * Handle a new WebSocket connection
   * @param {WebSocket} ws - The WebSocket connection
   * @param {string} clientAddr - Client address string
   * @returns {number} Connection ID or -1 on failure
   */
  acceptConnection(ws, clientAddr = 'websocket-client') {
    // Call WASM to create the connection
    const addrPtr = this._allocString(clientAddr);
    const connId = this.module._wasm_net_accept_connection(addrPtr);
    this.module._free(addrPtr);

    if (connId < 0) {
      console.error('[FreecivServer] Failed to accept connection');
      return -1;
    }

    // Store the mapping
    this.connections.set(ws, connId);
    this.connById.set(connId, ws);

    // Set up WebSocket handlers
    ws.binaryType = 'arraybuffer';

    ws.onmessage = (event) => {
      this._handleClientData(connId, event.data);
    };

    ws.onclose = (event) => {
      this._handleClientDisconnect(connId, ws);
    };

    ws.onerror = (error) => {
      console.error(`[FreecivServer] WebSocket error for conn ${connId}:`, error);
      this._handleClientDisconnect(connId, ws);
    };

    console.log(`[FreecivServer] Accepted connection ${connId} from ${clientAddr}`);
    this._emit('connection', { connId, clientAddr });

    return connId;
  }

  /**
   * Handle data received from a client
   */
  _handleClientData(connId, data) {
    let dataArray;

    if (data instanceof ArrayBuffer) {
      dataArray = new Uint8Array(data);
    } else if (typeof data === 'string') {
      // Convert string to bytes (for JSON protocol)
      const encoder = new TextEncoder();
      dataArray = encoder.encode(data);
    } else {
      dataArray = new Uint8Array(data);
    }

    // Allocate memory in WASM and copy data
    const ptr = this.module._malloc(dataArray.length);
    this.module.HEAPU8.set(dataArray, ptr);

    // Call WASM to process the data
    const result = this.module._wasm_net_receive_data(connId, ptr, dataArray.length);

    this.module._free(ptr);

    if (result < 0) {
      console.error(`[FreecivServer] Error receiving data for conn ${connId}`);
    }
  }

  /**
   * Handle client disconnect
   */
  _handleClientDisconnect(connId, ws) {
    this.connections.delete(ws);
    this.connById.delete(connId);

    // Notify WASM
    this.module._wasm_net_connection_closed(connId);

    console.log(`[FreecivServer] Client disconnected: conn ${connId}`);
    this._emit('disconnect', { connId });
  }

  /**
   * Handle server sending data to a client
   * Called from WASM via Module.onServerSendData
   */
  _handleServerSend(connId, data) {
    const ws = this.connById.get(connId);

    if (!ws) {
      console.warn(`[FreecivServer] No WebSocket for conn ${connId}`);
      return;
    }

    if (ws.readyState !== WebSocket.OPEN) {
      console.warn(`[FreecivServer] WebSocket not open for conn ${connId}`);
      return;
    }

    try {
      ws.send(data);
    } catch (e) {
      console.error(`[FreecivServer] Error sending to conn ${connId}:`, e);
    }
  }

  /**
   * Handle server closing a connection
   */
  _handleServerCloseConnection(connId, reason) {
    const ws = this.connById.get(connId);

    if (ws) {
      ws.close(1000, reason);
      this.connections.delete(ws);
      this.connById.delete(connId);
    }

    console.log(`[FreecivServer] Server closed conn ${connId}: ${reason}`);
  }

  /**
   * Handle server state change
   */
  _handleStateChange(newState) {
    const oldState = this.state;
    this.state = newState;

    const oldName = ServerStateNames[oldState] || 'UNKNOWN';
    const newName = ServerStateNames[newState] || 'UNKNOWN';

    console.log(`[FreecivServer] State change: ${oldName} -> ${newName}`);
    this._emit('stateChange', { oldState, newState, oldName, newName });
  }

  /**
   * Send a console command to the server
   * @param {string} command - The command to send
   */
  sendCommand(command) {
    const ptr = this._allocString(command);
    this.module._wasm_console_input(ptr);
    this.module._free(ptr);
  }

  /**
   * Get current server state
   * @returns {number} Current state
   */
  getState() {
    return this.module._wasm_server_get_state();
  }

  /**
   * Get state name
   * @param {number} state - State number
   * @returns {string} State name
   */
  getStateName(state = this.state) {
    return ServerStateNames[state] || 'UNKNOWN';
  }

  /**
   * Register an event handler
   * @param {string} event - Event name
   * @param {function} handler - Handler function
   */
  on(event, handler) {
    if (!this.eventHandlers.has(event)) {
      this.eventHandlers.set(event, []);
    }
    this.eventHandlers.get(event).push(handler);
  }

  /**
   * Remove an event handler
   * @param {string} event - Event name
   * @param {function} handler - Handler function
   */
  off(event, handler) {
    const handlers = this.eventHandlers.get(event);
    if (handlers) {
      const index = handlers.indexOf(handler);
      if (index !== -1) {
        handlers.splice(index, 1);
      }
    }
  }

  /**
   * Emit an event
   * @param {string} event - Event name
   * @param {object} data - Event data
   */
  _emit(event, data) {
    const handlers = this.eventHandlers.get(event);
    if (handlers) {
      for (const handler of handlers) {
        try {
          handler(data);
        } catch (e) {
          console.error(`[FreecivServer] Error in event handler for ${event}:`, e);
        }
      }
    }
  }

  /**
   * Allocate a string in WASM memory
   * @param {string} str - The string to allocate
   * @returns {number} Pointer to the string
   */
  _allocString(str) {
    const encoder = new TextEncoder();
    const bytes = encoder.encode(str + '\0');
    const ptr = this.module._malloc(bytes.length);
    this.module.HEAPU8.set(bytes, ptr);
    return ptr;
  }
}

/**
 * Create and configure a WebSocket server that connects to the WASM Freeciv server
 * This is for Node.js environments
 */
function createWebSocketServerBridge(wasmModule, wsServer) {
  const fcServer = new FreecivServer(wasmModule);

  wsServer.on('connection', (ws, req) => {
    const clientAddr = req.socket.remoteAddress || 'unknown';
    fcServer.acceptConnection(ws, clientAddr);
  });

  return fcServer;
}

/**
 * Create a client handler for browser environments
 * where WebSocket server is external and this bridges to it
 */
function createBrowserBridge(wasmModule) {
  return new FreecivServer(wasmModule);
}

// Export for different module systems
if (typeof module !== 'undefined' && module.exports) {
  // Node.js / CommonJS
  module.exports = {
    FreecivServer,
    ServerState,
    ServerStateNames,
    createWebSocketServerBridge,
    createBrowserBridge
  };
} else if (typeof window !== 'undefined') {
  // Browser global
  window.FreecivServer = FreecivServer;
  window.ServerState = ServerState;
  window.ServerStateNames = ServerStateNames;
  window.createBrowserBridge = createBrowserBridge;
}

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
#include <time.h>

#include <emscripten.h>

/* utility */
#include "log.h"
#include "mem.h"
#include "support.h"
#include "timing.h"

/* common */
#include "game.h"
#include "packets.h"

/* common/networking */
#include "connection.h"

/* server */
#include "aiiface.h"
#include "connecthand.h"
#include "console.h"
#include "meta.h"
#include "notify.h"
#include "plrhand.h"
#include "sernet.h"
#include "srv_main.h"
#include "stdinhand.h"
#include "voting.h"

/* server/scripting */
#include "script_server.h"

#include "wasm_server.h"
#include "wasm_net.h"

/* Current WASM server state */
static wasm_server_state current_wasm_state = WASM_SERVER_NOT_STARTED;

/* Flag to track if initialization has been done */
static bool wasm_initialized = FALSE;

/* Timing for periodic tasks */
static time_t last_ping_time = 0;
static time_t last_metaserver_time = 0;

/* Turn/phase state for running game */
static bool is_new_turn = TRUE;
static bool skip_mapimg = FALSE;
static bool need_send_pending_events = FALSE;
static int save_counter = 0;

/* Timers (from srv_main.c) */
static struct timer *wasm_eot_timer = NULL;
static struct timer *wasm_between_turns = NULL;

/**********************************************************************//**
  JavaScript callback declarations - implemented in JS
**************************************************************************/

/* Log message to JavaScript console */
EM_JS(void, js_log, (const char *msg), {
  console.log('[FreecivServer] ' + UTF8ToString(msg));
});

/* Notify JS of state change */
EM_JS(void, js_notify_state_change, (int state), {
  if (Module.onServerStateChange) {
    Module.onServerStateChange(state);
  }
});

/**********************************************************************//**
  Set WASM server state and notify JS
**************************************************************************/
static void set_wasm_state(wasm_server_state new_state)
{
  if (current_wasm_state != new_state) {
    current_wasm_state = new_state;
    js_notify_state_change((int)new_state);
  }
}

/**********************************************************************//**
  Process periodic server tasks (ping, metaserver, timeouts)
**************************************************************************/
static void wasm_process_periodic_tasks(void)
{
  time_t now = time(NULL);

  /* Pinging for statistics - every pingtime seconds */
  if (now > (last_ping_time + game.server.pingtime)) {
    /* Send ping times to all clients */
    conn_list_iterate(game.all_connections, pconn) {
      if (pconn->established && !pconn->server.is_closing) {
        /* Check for ping timeout */
        if (pconn->ping_time > game.server.pingtimeout) {
          if (pconn->access_level != ALLOW_HACK) {
            log_verbose("connection (%s) cut due to ping timeout",
                        conn_description(pconn));
            connection_close_server(pconn, _("ping timeout"));
          }
        }
      }
    } conn_list_iterate_end;
    last_ping_time = now;
  }

  /* Metaserver refresh - every 3 minutes or so */
  if (now > (last_metaserver_time + 180)) {
    (void) send_server_info_to_metaserver(META_REFRESH);
    last_metaserver_time = now;
  }
}

/**********************************************************************//**
  Check for turn timeout
**************************************************************************/
static bool wasm_check_turn_timeout(void)
{
  if (current_turn_timeout() > 0
      && S_S_RUNNING == server_state()
      && game.server.phase_timer
      && (timer_read_seconds(game.server.phase_timer)
          + game.server.additional_phase_seconds
          > game.tinfo.seconds_to_phasedone)) {
    return TRUE;
  }
  return FALSE;
}

/**********************************************************************//**
  Process all pending network data and game logic for one tick
  This is a non-blocking version of server_sniff_all_input()
**************************************************************************/
static enum server_events wasm_process_tick(void)
{
  /* Check for forced end of sniff (game start, etc.) */
  if (force_end_of_sniff) {
    force_end_of_sniff = FALSE;
    return S_E_FORCE_END_OF_SNIFF;
  }

  /* Process periodic tasks */
  wasm_process_periodic_tasks();

  /* Don't wait if timeout == -1 (auto games) */
  if (S_S_RUNNING == server_state() && game.info.timeout == -1) {
    call_ai_refresh();
    script_server_signal_emit("pulse");
    return S_E_END_OF_TURN_TIMEOUT;
  }

  /* Process all pending incoming data from connections */
  wasm_net_process_pending();

  /* Flush outgoing data to all connections */
  flush_packets();

  /* Call AI refresh and emit pulse signal */
  call_ai_refresh();
  script_server_signal_emit("pulse");

  /* Check for turn timeout */
  if (wasm_check_turn_timeout()) {
    return S_E_END_OF_TURN_TIMEOUT;
  }

  /* Check for timer-based autosave */
  if ((game.server.autosaves & (1 << AS_TIMER))
      && S_S_RUNNING == server_state()
      && game.server.save_timer
      && (timer_read_seconds(game.server.save_timer)
          >= game.server.save_frequency * 60)) {
    save_game_auto("Timer", AS_TIMER);
    game.server.save_timer = timer_renew(game.server.save_timer,
                                         TIMER_USER, TIMER_ACTIVE,
                                         NULL);
    timer_start(game.server.save_timer);
  }

  return S_E_OTHERWISE;
}

/**********************************************************************//**
  Handle pregame state (S_S_INITIAL) - waiting for players
**************************************************************************/
static wasm_server_state wasm_tick_pregame(void)
{
  enum server_events event = wasm_process_tick();

  if (event == S_E_FORCE_END_OF_SNIFF) {
    /* Game is ready to start */
    return WASM_SERVER_STARTING_GAME;
  }

  return WASM_SERVER_WAITING_FOR_PLAYERS;
}

/**********************************************************************//**
  Handle running game state (S_S_RUNNING)
  This implements the turn/phase loop in a non-blocking way
**************************************************************************/
static wasm_server_state wasm_tick_running(void)
{
  enum server_events event;

  /* Process one tick of the game */
  event = wasm_process_tick();

  /* Check if turn/phase should end */
  if (event == S_E_END_OF_TURN_TIMEOUT || event == S_E_FORCE_END_OF_SNIFF) {
    /* Time to advance the phase/turn */
    return WASM_SERVER_BETWEEN_TURNS;
  }

  /* Check if game is over */
  if (S_S_OVER == server_state()) {
    return WASM_SERVER_GAME_OVER;
  }

  return WASM_SERVER_RUNNING_TURN;
}

/**********************************************************************//**
  Handle game over state (S_S_OVER)
**************************************************************************/
static wasm_server_state wasm_tick_game_over(void)
{
  /* Process network to let players disconnect */
  wasm_process_tick();

  /* If all players disconnected, could restart or shutdown */
  if (conn_list_size(game.est_connections) == 0) {
    if (srvarg.exit_on_end) {
      return WASM_SERVER_SHUTTING_DOWN;
    }
    /* Could implement restart logic here */
  }

  return WASM_SERVER_GAME_OVER;
}

/**********************************************************************//**
  Initialize the WASM server
**************************************************************************/
EMSCRIPTEN_KEEPALIVE
void wasm_server_init(void)
{
  if (wasm_initialized) {
    js_log("Server already initialized");
    return;
  }

  js_log("Initializing Freeciv WASM server...");

  set_wasm_state(WASM_SERVER_INITIALIZING);

  /* Initialize server internals (from srv_init) */
  srv_init();

  /* Initialize WASM networking layer */
  wasm_net_init();

  /* Initialize timers */
  wasm_eot_timer = timer_new(TIMER_CPU, TIMER_ACTIVE, "wasm-eot");
  last_ping_time = time(NULL);
  last_metaserver_time = time(NULL);

  /* Note: We don't call srv_prepare() fully here because it tries to
   * open sockets. Instead we do minimal initialization. */

  /* Initialize connections */
  init_connections();

  /* Initialize game state */
  set_server_state(S_S_INITIAL);

  /* Load a script file if specified */
  if (srvarg.script_filename != NULL) {
    (void) read_init_script(NULL, srvarg.script_filename, TRUE, FALSE);
  }

  /* Fill AI players */
  (void) aifill(game.info.aifill);

  if (!game_was_started()) {
    event_cache_clear();
  }

  wasm_initialized = TRUE;
  set_wasm_state(WASM_SERVER_WAITING_FOR_PLAYERS);

  js_log("Freeciv WASM server initialized and waiting for players");
}

/**********************************************************************//**
  Main tick function - call from JavaScript event loop
**************************************************************************/
EMSCRIPTEN_KEEPALIVE
wasm_server_state wasm_server_tick(void)
{
  if (!wasm_initialized) {
    return WASM_SERVER_NOT_STARTED;
  }

  wasm_server_state new_state = current_wasm_state;

  switch (current_wasm_state) {
  case WASM_SERVER_NOT_STARTED:
    /* Should call wasm_server_init() first */
    break;

  case WASM_SERVER_INITIALIZING:
    /* Initialization in progress */
    break;

  case WASM_SERVER_WAITING_FOR_PLAYERS:
    new_state = wasm_tick_pregame();
    break;

  case WASM_SERVER_STARTING_GAME:
    /* Transition to running state */
    /* This is where srv_ready() and beginning of srv_running() would happen */
    if (S_S_RUNNING > server_state()) {
      /* srv_ready() sets server state to S_S_RUNNING */
      /* Note: This would need the actual srv_ready() call */
      js_log("Game starting...");

      /* Initialize turn state */
      is_new_turn = game.info.is_new_game;
      skip_mapimg = !game.info.is_new_game;
      need_send_pending_events = !game.info.is_new_game;
      save_counter = game.info.is_new_game ? 1 : 0;

      game.info.is_new_game = FALSE;

      timer_start(wasm_eot_timer);

      if (game.server.autosaves & (1 << AS_TIMER)) {
        game.server.save_timer = timer_renew(game.server.save_timer,
                                             TIMER_USER, TIMER_ACTIVE,
                                             NULL);
        timer_start(game.server.save_timer);
      }
    }
    new_state = WASM_SERVER_RUNNING_TURN;
    break;

  case WASM_SERVER_RUNNING_TURN:
    new_state = wasm_tick_running();
    break;

  case WASM_SERVER_BETWEEN_TURNS:
    /* Handle end of phase/turn logic */
    /* This would call end_phase(), possibly end_turn() */
    js_log("Processing end of turn...");
    new_state = WASM_SERVER_RUNNING_TURN;

    /* Check for game over */
    if (S_S_OVER == server_state() || check_for_game_over()) {
      set_server_state(S_S_OVER);
      new_state = WASM_SERVER_GAME_OVER;
    }
    break;

  case WASM_SERVER_GAME_OVER:
    new_state = wasm_tick_game_over();
    break;

  case WASM_SERVER_SHUTTING_DOWN:
    /* Cleanup and exit */
    break;
  }

  if (new_state != current_wasm_state) {
    set_wasm_state(new_state);
  }

  return current_wasm_state;
}

/**********************************************************************//**
  Get current server state
**************************************************************************/
EMSCRIPTEN_KEEPALIVE
wasm_server_state wasm_server_get_state(void)
{
  return current_wasm_state;
}

/**********************************************************************//**
  Graceful shutdown
**************************************************************************/
EMSCRIPTEN_KEEPALIVE
void wasm_server_shutdown(void)
{
  js_log("Shutting down Freeciv WASM server...");

  set_wasm_state(WASM_SERVER_SHUTTING_DOWN);

  /* Close all connections */
  conn_list_iterate(game.all_connections, pconn) {
    connection_close_server(pconn, _("server shutdown"));
  } conn_list_iterate_end;

  /* Cleanup */
  wasm_net_shutdown();

  if (wasm_eot_timer != NULL) {
    timer_destroy(wasm_eot_timer);
    wasm_eot_timer = NULL;
  }
  if (wasm_between_turns != NULL) {
    timer_destroy(wasm_between_turns);
    wasm_between_turns = NULL;
  }

  wasm_initialized = FALSE;

  js_log("Freeciv WASM server shutdown complete");
}

/**********************************************************************//**
  Console command input (replaces stdin)
**************************************************************************/
EMSCRIPTEN_KEEPALIVE
void wasm_console_input(const char *command)
{
  if (command != NULL && command[0] != '\0') {
    handle_stdin_input(NULL, (char *)command);
  }
}

#endif /* __EMSCRIPTEN__ */

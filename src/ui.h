#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include "wifi.h"

/*
 * ui.h — ncurses rendering and input handling
 *
 * The UI layer owns all terminal state.  It knows nothing about D-Bus;
 * it works entirely with WifiNetwork structs supplied by wifi.c and
 * with the passphrase callback contract expected by agent.c.
 *
 * Screen layout (53 × 40):
 *
 *   Row  0     ╔═ title bar ═╗
 *   Row  1     ║ status line ║
 *   Row  2     ╟─────────────╢
 *   Row  3-36  ║ network list║   (up to 34 rows of networks)
 *   Row 37     ╟─────────────╢
 *   Row 38-39  ║  key hints  ║
 */

#define UI_COLS        53
#define UI_ROWS        40
#define UI_LIST_ROWS   34   /* rows available for the network list  */

/* Possible states the UI can be in. */
typedef enum {
    UI_STATE_LIST,          /* browsing the network list            */
    UI_STATE_SCANNING,      /* scan in progress, list locked        */
    UI_STATE_PASSPHRASE,    /* password prompt overlay is visible   */
    UI_STATE_CONNECTING,    /* spinner shown while connecting       */
    UI_STATE_MESSAGE,       /* transient success / error message    */
} UiState;

/* Initialise ncurses.  Must be called before any other ui_* function.
 * Returns 0 on success, -1 if the terminal is too small. */
int  ui_init(void);

/* Tear down ncurses and restore the terminal. */
void ui_cleanup(void);

/* Replace the displayed network list. Resets the cursor to 0 if
 * the list length changed. */
void ui_set_networks(const WifiNetwork *networks, int count);

/* Update the status line text (e.g. "Scanning…", "Connected"). */
void ui_set_status(const char *fmt, ...);

/* Switch to a given UI state and redraw. */
void ui_set_state(UiState state);

/* Redraw the entire screen from current state. */
void ui_draw(void);

/* Block waiting for a keypress and return it.
 * Handles terminal resize internally.
 * Returns the raw ncurses key value (KEY_UP, KEY_DOWN, 'q', '\n', …). */
int ui_get_key(void);

/* Return the index of the currently highlighted network, or -1 if the
 * list is empty. */
int ui_get_cursor(void);

/*
 * Show the passphrase prompt overlay for the given SSID.
 * Blocks until the user confirms or cancels.
 *
 * On confirmation writes the passphrase into buf (NUL-terminated,
 * at most buf_len-1 chars) and returns 0.
 * On cancel (Esc or 'q') returns -ECANCELED without modifying buf.
 *
 * This is also the function registered as the AgentPassphraseCallback
 * so agent.c can call it directly.
 */
int ui_prompt_passphrase(const char *ssid, char *buf, int buf_len);

/* Show a Y/N confirmation dialog with the given prompt.
 * Returns true if the user pressed Y, false for N or Esc. */
bool ui_confirm(const char *prompt);

/* Display a transient message (success or error) for a short duration,
 * then return to UI_STATE_LIST.  duration_ms of 0 waits for a keypress. */
void ui_show_message(const char *msg, bool is_error, int duration_ms);

#endif /* UI_H */

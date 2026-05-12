#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <ncurses.h>

#include "dbus.h"
#include "wifi.h"
#include "agent.h"
#include "ui.h"

/*
 * main.c — entry point and event loop
 *
 * Startup order:
 *   1. dbus_init()          — open the system bus
 *   2. wifi_init()          — locate the iwd station object
 *   3. ui_init()            — set up ncurses
 *   4. agent_set_passphrase_callback() — wire agent → ui
 *   5. agent_init()         — register agent with iwd
 *   6. Initial scan + draw
 *   7. Event loop
 *
 * Teardown is the reverse, via atexit() handlers so that the
 * terminal is always restored even on unexpected exit.
 */

static void do_cleanup(void) {
    agent_cleanup();
    ui_cleanup();
    wifi_cleanup();
    dbus_cleanup();
}

static void do_scan(void) {
    int count;

    ui_set_state(UI_STATE_SCANNING);
    ui_set_status("Scanning...");
    ui_draw();

    if (wifi_scan() < 0) {
        ui_show_message("Scan failed. Is iwd running?", /*is_error=*/1, 0);
        return;
    }

    const WifiNetwork *nets = wifi_get_networks(&count);
    ui_set_networks(nets, count);
    ui_set_state(UI_STATE_LIST);

    if (count == 0)
        ui_set_status("No networks found. Press R to rescan.");
    else
        ui_set_status("%d network%s found.", count, count == 1 ? "" : "s");

    ui_draw();
}

static void do_connect(void) {
    int idx = ui_get_cursor();
    if (idx < 0)
        return;

    ui_set_state(UI_STATE_CONNECTING);
    ui_set_status("Connecting...");
    ui_draw();

    int rc = wifi_connect(idx);
    if (rc == 0) {
        ui_show_message("Connected successfully.", /*is_error=*/0, 1500);
        do_scan(); /* refresh list to show connected state */
    } else if (rc == -ECANCELED) {
        /* user aborted passphrase prompt — just go back to list */
        ui_set_state(UI_STATE_LIST);
        ui_draw();
    } else {
        ui_show_message("Connection failed.", /*is_error=*/1, 0);
    }
}

int main(void) {
    atexit(do_cleanup);

    if (dbus_init() < 0) {
        fprintf(stderr, "calwifi: could not connect to system bus\n");
        return EXIT_FAILURE;
    }

    if (wifi_init() < 0) {
        fprintf(stderr, "calwifi: could not find a WiFi adapter via iwd\n");
        return EXIT_FAILURE;
    }

    if (ui_init() < 0) {
        fprintf(stderr, "calwifi: terminal too small (need %dx%d)\n",
                UI_COLS, UI_ROWS);
        return EXIT_FAILURE;
    }

    agent_set_passphrase_callback(ui_prompt_passphrase);

    if (agent_init() < 0) {
        ui_show_message("Warning: agent registration failed.\n"
                        "PSK networks may not prompt for a password.",
                        /*is_error=*/1, 0);
    }

    do_scan();

    /* ── event loop ── */
    for (;;) {
        int key = ui_get_key();

        switch (key) {
        case 'q':
        case 'Q':
            return EXIT_SUCCESS;

        case 'f':
        case 'F': {
            int idx = ui_get_cursor();
            if (idx < 0) break;
            int count;
            const WifiNetwork *nets = wifi_get_networks(&count);
            if (!nets[idx].known) {
                ui_show_message("Not a known network.", /*is_error=*/0, 1500);
                break;
            }
            char prompt[64];
            snprintf(prompt, sizeof(prompt), "Forget %.*s?",
                     (int)sizeof(prompt) - 10, nets[idx].ssid);
            if (ui_confirm(prompt)) {
                if (wifi_forget(idx) == 0)
                    ui_show_message("Network forgotten.", /*is_error=*/0, 1500);
                else
                    ui_show_message("Forget failed.", /*is_error=*/1, 0);
                do_scan();
            }
            break;
        }

        case 'd':
        case 'D':
            ui_set_state(UI_STATE_CONNECTING); /* reuse spinner */
            ui_set_status("Disconnecting...");
            ui_draw();
            if (wifi_disconnect() == 0) {
                ui_show_message("Disconnected.", /*is_error=*/0, 1500);
            } else {
                ui_show_message("Disconnect failed.", /*is_error=*/1, 0);
            }
            do_scan();
            break;

        case 'r':
        case 'R':
            do_scan();
            break;

        case '\n':
        case KEY_ENTER:
            do_connect();
            break;

        case KEY_UP:
        case 'k':
            /* cursor movement is handled inside ui_get_key / ui_draw;
             * a redraw is enough here */
            ui_draw();
            break;

        case KEY_DOWN:
        case 'j':
            ui_draw();
            break;

        default:
            break;
        }
    }
}

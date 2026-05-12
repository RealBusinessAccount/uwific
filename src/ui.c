/* explicit_bzero requires _GNU_SOURCE; nanosleep requires _POSIX_C_SOURCE >= 199309L.
 * Define both before any system header so the right prototypes are exposed. */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 199309L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <stdbool.h>
#include <time.h>
#include <ncurses.h>

#include "ui.h"
#include "wifi.h"

/*
 * ui.c — ncurses rendering and input handling
 *
 * All terminal state lives here. No D-Bus, no iwd knowledge — the UI
 * works entirely with WifiNetwork structs and the passphrase callback
 * contract from agent.h.
 *
 * Layout (53 × 40):
 *
 *   Row  0    ╔═══ Calculinux WiFi Manager ══════════╗
 *   Row  1    ║ Status: <status text>                ║
 *   Row  2    ╠═══════════════════════════════════════╣
 *   Row  3    ║ Networks:                             ║
 *   Row  4    ╠═══════════════════════════════════════╣
 *   Rows 5-38 ║ <network list, up to 34 rows>         ║
 *   Row 39    ║ [↑↓] Navigate  [Enter] Connect  ...  ║
 *
 * Colour pairs:
 *   1 — title bar        (black on green)
 *   2 — selected row     (black on white)
 *   3 — signal strong    (green)
 *   4 — signal good      (yellow)
 *   5 — signal fair      (yellow)
 *   6 — signal weak/none (red)
 *   7 — known tag        (cyan)
 *   8 — connected tag    (green, bold)
 *   9 — error message    (black on red)
 *  10 — status bar       (default, dim)
 *  11 — border / chrome  (default)
 */

#define ROW_TITLE       0
#define ROW_STATUS      1
#define ROW_DIVIDER1    2
#define ROW_NET_HEADER  3
#define ROW_DIVIDER2    4
#define ROW_LIST_START  5

/* These are computed at runtime from LINES once ncurses is initialised. */
static int g_row_keyhints  = 38;  /* first hint row  */
static int g_row_keyhints2 = 39;  /* second hint row */
static int g_list_rows     = 33;

#define COL_CURSOR      1
#define COL_SSID        3
#define COL_SIGNAL     30
#define COL_SECURITY   35
#define COL_KNOWN      43

#define SSID_DISPLAY_MAX  25   /* chars before truncation with … */
#define PASSPHRASE_BUFLEN 64

/* colour pair IDs */
#define CP_TITLE     1
#define CP_SELECTED  2
#define CP_SIG_FULL  3
#define CP_SIG_GOOD  4
#define CP_SIG_FAIR  5
#define CP_SIG_WEAK  6
#define CP_KNOWN     7
#define CP_CONNECTED 8
#define CP_ERROR     9
#define CP_STATUS   10
#define CP_BORDER   11

/* spinner frames — ASCII only, no UTF-8 */
static const char *SPINNER[] = { "-", "\\", "|", "/" };
#define SPINNER_FRAMES (int)(sizeof(SPINNER)/sizeof(SPINNER[0]))

/* ── module state ───────────────────────────────────────────────────────── */

static UiState       g_state        = UI_STATE_LIST;
static char          g_status[128]  = "Ready.";
static WifiNetwork   g_networks[WIFI_MAX_NETWORKS];
static int           g_net_count    = 0;
static int           g_cursor       = 0;
static int           g_list_offset  = 0;   /* first visible row in list */

/* ── colour helpers ─────────────────────────────────────────────────────── */

static void init_colors(void)
{
    start_color();
    use_default_colors();

    init_pair(CP_TITLE,    COLOR_BLACK,  COLOR_GREEN);
    init_pair(CP_SELECTED, COLOR_BLACK,  COLOR_WHITE);
    init_pair(CP_SIG_FULL, COLOR_GREEN,  -1);
    init_pair(CP_SIG_GOOD, COLOR_GREEN,  -1);
    init_pair(CP_SIG_FAIR, COLOR_YELLOW, -1);
    init_pair(CP_SIG_WEAK, COLOR_RED,    -1);
    init_pair(CP_KNOWN,    COLOR_CYAN,   -1);
    init_pair(CP_CONNECTED,COLOR_GREEN,  -1);
    init_pair(CP_ERROR,    COLOR_WHITE,  COLOR_RED);
    init_pair(CP_STATUS,   -1,           -1);
    init_pair(CP_BORDER,   -1,           -1);
}

static int signal_color(SignalStrength s)
{
    switch (s) {
    case SIGNAL_STRONG: return CP_SIG_FULL;
    case SIGNAL_GOOD:   return CP_SIG_GOOD;
    case SIGNAL_FAIR:   return CP_SIG_FAIR;
    default:            return CP_SIG_WEAK;
    }
}

/* ── signal bar rendering ───────────────────────────────────────────────── */

static const char *signal_bars(SignalStrength s)
{
    switch (s) {
    case SIGNAL_STRONG: return "####";
    case SIGNAL_GOOD:   return "###+";
    case SIGNAL_FAIR:   return "##++";
    case SIGNAL_WEAK:   return "#+++ ";
    default:            return "++++";
    }
}

/* ── init / cleanup ─────────────────────────────────────────────────────── */

int ui_init(void)
{
    initscr();

    /* Require at least enough rows for chrome + a few network entries.
     * ROW_LIST_START=5, keyhints=1, divider=1, so minimum useful height is 9. */
    if (LINES < 9 || COLS < 20) {
        endwin();
        return -1;
    }

    /* Key hints always sit on the last row; list fills everything between. */
    g_row_keyhints2 = LINES - 1;
    g_row_keyhints  = LINES - 2;
    g_list_rows     = g_row_keyhints - ROW_LIST_START - 1; /* -1 for divider */

    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    set_escdelay(50);  /* shorten Esc delay from the default 1000ms */

    if (has_colors())
        init_colors();

    return 0;
}

void ui_cleanup(void)
{
    if (!isendwin()) {
        clear();
        refresh();
        endwin();
    }
}

/* ── state management ───────────────────────────────────────────────────── */

void ui_set_networks(const WifiNetwork *networks, int count)
{
    int old_count = g_net_count;

    g_net_count = (count > WIFI_MAX_NETWORKS) ? WIFI_MAX_NETWORKS : count;
    memcpy(g_networks, networks, g_net_count * sizeof(WifiNetwork));

    /* Reset cursor only if the list length changed to avoid jarring jumps
     * when the user rescans with the same set of networks visible. */
    if (g_net_count != old_count) {
        g_cursor      = 0;
        g_list_offset = 0;
    }

    /* Clamp cursor in case the new list is shorter. */
    if (g_cursor >= g_net_count)
        g_cursor = (g_net_count > 0) ? g_net_count - 1 : 0;
}

void ui_set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status, sizeof(g_status), fmt, ap);
    va_end(ap);
}

void ui_set_state(UiState state)
{
    g_state = state;
}

int ui_get_cursor(void)
{
    return (g_net_count > 0) ? g_cursor : -1;
}

/* ── drawing ────────────────────────────────────────────────────────────── */

static void draw_title(void)
{
    attron(COLOR_PAIR(CP_TITLE) | A_BOLD);
    mvhline(ROW_TITLE, 0, ' ', UI_COLS);
    mvprintw(ROW_TITLE, 2, "Calculinux WiFi Manager");
    attroff(COLOR_PAIR(CP_TITLE) | A_BOLD);
}

static void draw_status(void)
{
    attron(COLOR_PAIR(CP_STATUS));
    mvhline(ROW_STATUS, 0, ' ', UI_COLS);
    mvprintw(ROW_STATUS, 1, " Status: ");

    if (g_state == UI_STATE_SCANNING) {
        static int frame = 0;
        printw("%s Scanning...", SPINNER[frame % SPINNER_FRAMES]);
        frame++;
    } else {
        printw("%.*s", UI_COLS - 11, g_status);
    }
    attroff(COLOR_PAIR(CP_STATUS));
}

static void draw_divider(int row)
{
    attron(COLOR_PAIR(CP_BORDER));
    mvhline(row, 0, ACS_HLINE, UI_COLS);
    attroff(COLOR_PAIR(CP_BORDER));
}

static void draw_network_row(int screen_row, int net_idx, bool selected)
{
    const WifiNetwork *n = &g_networks[net_idx];
    char ssid_display[SSID_DISPLAY_MAX + 2]; /* +2 for … and NUL */

    /* Truncate SSID if necessary */
    if ((int)strlen(n->ssid) > SSID_DISPLAY_MAX) {
        memcpy(ssid_display, n->ssid, SSID_DISPLAY_MAX - 1);
        ssid_display[SSID_DISPLAY_MAX - 1] = '\xe2'; /* UTF-8 … = e2 80 a6 */
        ssid_display[SSID_DISPLAY_MAX]     = '\x80';
        ssid_display[SSID_DISPLAY_MAX + 1] = '\xa6';
        ssid_display[SSID_DISPLAY_MAX + 2] = '\0';
    } else {
        strncpy(ssid_display, n->ssid, sizeof(ssid_display) - 1);
        ssid_display[sizeof(ssid_display) - 1] = '\0';
    }

    if (selected)
        attron(COLOR_PAIR(CP_SELECTED) | A_BOLD);
    else
        attroff(A_BOLD);

    /* Clear the row */
    mvhline(screen_row, 0, ' ', UI_COLS);

    /* Cursor indicator */
    mvprintw(screen_row, COL_CURSOR, selected ? ">" : " ");

    /* SSID */
    mvprintw(screen_row, COL_SSID, "%-*s", SSID_DISPLAY_MAX, ssid_display);

    if (selected)
        attroff(COLOR_PAIR(CP_SELECTED) | A_BOLD);

    /* Signal bars (coloured independently of selection highlight) */
    if (selected) attron(COLOR_PAIR(CP_SELECTED));
    else          attron(COLOR_PAIR(signal_color(n->signal)));
    mvprintw(screen_row, COL_SIGNAL, "%s", signal_bars(n->signal));
    if (selected) attroff(COLOR_PAIR(CP_SELECTED));
    else          attroff(COLOR_PAIR(signal_color(n->signal)));

    /* Security type */
    if (selected) attron(COLOR_PAIR(CP_SELECTED));
    mvprintw(screen_row, COL_SECURITY, "%-7s", n->security);
    if (selected) attroff(COLOR_PAIR(CP_SELECTED));

    /* Known / connected tags */
    if (n->connected) {
        attron(COLOR_PAIR(CP_CONNECTED) | A_BOLD);
        mvprintw(screen_row, COL_KNOWN, "[*]");
        attroff(COLOR_PAIR(CP_CONNECTED) | A_BOLD);
    } else if (n->known) {
        attron(COLOR_PAIR(CP_KNOWN));
        mvprintw(screen_row, COL_KNOWN, "[k]");
        attroff(COLOR_PAIR(CP_KNOWN));
    }
}

static void draw_network_list(void)
{
    /* Section header */
    attron(A_DIM);
    mvprintw(ROW_NET_HEADER, 1, "Networks:");
    attroff(A_DIM);

    int visible = g_list_rows;
    if (visible > g_net_count) visible = g_net_count;

    for (int i = 0; i < visible; i++) {
        int net_idx    = g_list_offset + i;
        int screen_row = ROW_LIST_START + i;
        draw_network_row(screen_row, net_idx, net_idx == g_cursor);
    }

    /* Clear any leftover rows below the list */
    for (int i = visible; i < g_list_rows; i++)
        mvhline(ROW_LIST_START + i, 0, ' ', COLS);

    /* Scroll indicators */
    if (g_list_offset > 0) {
        attron(A_DIM);
        mvprintw(ROW_LIST_START, COLS - 2, "^^");
        attroff(A_DIM);
    }
    if (g_list_offset + g_list_rows < g_net_count) {
        attron(A_DIM);
        mvprintw(ROW_LIST_START + g_list_rows - 1, COLS - 2, "vv");
        attroff(A_DIM);
    }
}

static void draw_keyhints(void)
{
    attron(A_DIM);
    mvhline(g_row_keyhints,  0, ' ', COLS);
    mvhline(g_row_keyhints2, 0, ' ', COLS);
    mvprintw(g_row_keyhints,  1, "[Up/Dn/jk] Navigate  [Enter] Connect");
    mvprintw(g_row_keyhints2, 1, "[D] Disconnect  [F] Forget  [R] Rescan  [Q] Quit");
    attroff(A_DIM);
}

static void draw_connecting_overlay(void)
{
    static int frame = 0;
    int mid_row = LINES / 2;
    int mid_col = COLS / 2 - 10;

    attron(A_REVERSE | A_BOLD);
    mvprintw(mid_row, mid_col, "  %s Connecting...  ",
             SPINNER[frame % SPINNER_FRAMES]);
    attroff(A_REVERSE | A_BOLD);
    frame++;
}

void ui_draw(void)
{
    clear();

    draw_title();
    draw_status();
    draw_divider(ROW_DIVIDER1);
    draw_network_list();
    draw_divider(g_row_keyhints - 1);
    draw_keyhints();

    if (g_state == UI_STATE_CONNECTING)
        draw_connecting_overlay();

    refresh();
}

/* ── input handling ─────────────────────────────────────────────────────── */

/*
 * Scroll the list so the cursor is always visible, maintaining a
 * one-row lookahead/lookbehind where possible.
 */
static void clamp_scroll(void)
{
    if (g_cursor < g_list_offset)
        g_list_offset = g_cursor;
    if (g_cursor >= g_list_offset + g_list_rows)
        g_list_offset = g_cursor - g_list_rows + 1;
}

int ui_get_key(void)
{
    int key = getch();

    switch (key) {
    case KEY_UP:
    case 'k':
        if (g_cursor > 0) {
            g_cursor--;
            clamp_scroll();
            ui_draw();
        }
        break;

    case KEY_DOWN:
    case 'j':
        if (g_cursor < g_net_count - 1) {
            g_cursor++;
            clamp_scroll();
            ui_draw();
        }
        break;

    case KEY_RESIZE:
        ui_draw();
        break;

    default:
        break;
    }

    return key;
}

/* ── passphrase prompt ──────────────────────────────────────────────────── */

int ui_prompt_passphrase(const char *ssid, char *buf, int buf_len)
{
    int box_w   = UI_COLS - 4;
    int box_h   = 7;
    int box_y   = (LINES - box_h) / 2;
    int box_x   = 2;
    int input_y = box_y + 4;
    int input_x = box_x + 2;
    int input_w = box_w - 4;

    /* Save the current screen so we can restore it on cancel. */
    WINDOW *overlay = newwin(box_h, box_w, box_y, box_x);
    if (!overlay)
        return -ENOMEM;

    keypad(overlay, TRUE);
    box(overlay, 0, 0);

    /* Title */
    wattron(overlay, A_BOLD);
    mvwprintw(overlay, 1, 2, "Connect to network");
    wattroff(overlay, A_BOLD);

    /* SSID (truncated to fit) */
    char ssid_trunc[UI_COLS];
    snprintf(ssid_trunc, sizeof(ssid_trunc), "%-*.*s",
             box_w - 4, box_w - 4, ssid);
    mvwprintw(overlay, 2, 2, "%s", ssid_trunc);

    mvwprintw(overlay, 3, 2, "Password:");

    wrefresh(overlay);

    /* Echo field — we manage it manually for masking. */
    char input[PASSPHRASE_BUFLEN] = {0};
    int  pos   = 0;
    int  key;

    curs_set(1);
    echo();  /* temporarily enable echo so the cursor advances */
    noecho(); /* actually keep masked — we print '*' ourselves */

    while (1) {
        /* Redraw the input field */
        wmove(overlay, input_y - box_y, input_x - box_x);
        for (int i = 0; i < input_w; i++)
            waddch(overlay, i < pos ? '*' : ' ');
        wmove(overlay, input_y - box_y, input_x - box_x + pos);
        wrefresh(overlay);

        key = wgetch(overlay);

        if (key == '\n' || key == KEY_ENTER) {
            if (pos == 0)
                continue;   /* don't allow empty passphrase */
            break;
        }

        if (key == 27 /* Esc */ || key == 'q') {
            curs_set(0);
            delwin(overlay);
            ui_draw();
            return -ECANCELED;
        }

        if ((key == KEY_BACKSPACE || key == 127 || key == '\b') && pos > 0) {
            input[--pos] = '\0';
            continue;
        }

        if (key >= 32 && key <= 126 && pos < buf_len - 1) {
            input[pos++] = (char)key;
            input[pos]   = '\0';
        }
    }

    curs_set(0);
    delwin(overlay);
    ui_draw();

    strncpy(buf, input, buf_len - 1);
    buf[buf_len - 1] = '\0';

    /* Clear the stack copy of the passphrase */
    explicit_bzero(input, sizeof(input));

    return 0;
}

/* ── confirm dialog ─────────────────────────────────────────────────────── */

bool ui_confirm(const char *prompt)
{
    int prompt_len = (int)strlen(prompt);
    int box_w      = (prompt_len + 4 < COLS) ? prompt_len + 4 : COLS - 2;
    /* Ensure room for the Y/N line which is always 12 chars */
    if (box_w < 16) box_w = 16;
    int box_h  = 5;
    int box_y  = (LINES - box_h) / 2;
    int box_x  = (COLS  - box_w) / 2;

    WINDOW *dlg = newwin(box_h, box_w, box_y, box_x);
    if (!dlg)
        return false;

    keypad(dlg, TRUE);
    box(dlg, 0, 0);

    mvwprintw(dlg, 1, 2, "%.*s", box_w - 4, prompt);

    /* Y/N hint centred on row 3 */
    int hint_x = (box_w - 12) / 2;
    wattron(dlg, A_BOLD);
    mvwprintw(dlg, 3, hint_x, "[Y]es  [N]o");
    wattroff(dlg, A_BOLD);

    wrefresh(dlg);
    flushinp();

    bool result = false;
    int key;
    while (1) {
        key = wgetch(dlg);
        if (key == 'y' || key == 'Y') { result = true;  break; }
        if (key == 'n' || key == 'N') { result = false; break; }
        if (key == 27 /* Esc */)      { result = false; break; }
    }

    delwin(dlg);
    clear();
    ui_draw();
    return result;
}

/* ── transient message ──────────────────────────────────────────────────── */

void ui_show_message(const char *msg, bool is_error, int duration_ms)
{
    int cp      = is_error ? CP_ERROR : CP_CONNECTED;
    int mid_row = LINES / 2;
    int msg_len = (int)strlen(msg);
    int box_w   = (msg_len + 4 < COLS) ? msg_len + 4 : COLS - 2;
    int box_x   = (COLS - box_w) / 2;

    attron(COLOR_PAIR(cp) | A_BOLD);
    mvhline(mid_row,     box_x, ' ', box_w);
    mvhline(mid_row + 1, box_x, ' ', box_w);
    mvhline(mid_row + 2, box_x, ' ', box_w);
    mvprintw(mid_row + 1, box_x + 2, "%.*s", box_w - 4, msg);
    attroff(COLOR_PAIR(cp) | A_BOLD);
    refresh();

    if (duration_ms > 0) {
        struct timespec ts = {
            .tv_sec  = duration_ms / 1000,
            .tv_nsec = (duration_ms % 1000) * 1000000L
        };
        nanosleep(&ts, NULL);
    } else {
        /* Flush any keys that piled up (e.g. rapid R presses) before
         * waiting, so stale input doesn't immediately re-trigger an action. */
        flushinp();
        getch();
    }

    g_state = UI_STATE_LIST;
    clear();
    ui_draw();
}

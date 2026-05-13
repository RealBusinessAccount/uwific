#ifndef WIFI_H
#define WIFI_H

#include <stdbool.h>
#include <stdint.h>

/*
 * wifi.h — iwd network operations
 *
 * Wraps net.connman.iwd Adapter, Station, and Network interfaces.
 * Builds and owns the adapter and network lists; the UI layer reads
 * from those lists but never writes to them directly.
 */

#define WIFI_SSID_MAX         33    /* 32-byte SSID + NUL                  */
#define WIFI_PATH_MAX        128    /* D-Bus object path                   */
#define WIFI_SECURITY_MAX     16    /* "psk", "8021x", "open", etc.        */
#define WIFI_MAX_NETWORKS     32    /* cap on displayed networks           */
#define WIFI_ADAPTER_NAME_MAX 64    /* adapter name e.g. "wlan0"           */
#define WIFI_MAX_ADAPTERS      8    /* cap on adapters                     */

/* Signal strength bucket used by the UI for the bar indicator. */
typedef enum {
    SIGNAL_NONE   = 0,
    SIGNAL_WEAK   = 1,
    SIGNAL_FAIR   = 2,
    SIGNAL_GOOD   = 3,
    SIGNAL_STRONG = 4
} SignalStrength;

/* One WiFi adapter as presented to the UI. */
typedef struct {
    char object_path[WIFI_PATH_MAX];      /* net.connman.iwd.Adapter path  */
    char station_path[WIFI_PATH_MAX];     /* associated Station path        */
    char name[WIFI_ADAPTER_NAME_MAX];     /* phy name e.g. "phy0"          */
    char ifname[WIFI_ADAPTER_NAME_MAX];   /* iface name e.g. "wlan0"       */
    bool powered;
    bool active;                          /* currently selected by uwific  */
} WifiAdapter;

/* One visible network as presented to the UI. */
typedef struct {
    char           ssid[WIFI_SSID_MAX];
    char           object_path[WIFI_PATH_MAX];       /* for Connect()  */
    char           known_network_path[WIFI_PATH_MAX]; /* for Forget()  */
    char           security[WIFI_SECURITY_MAX];      /* "psk", "open"  */
    SignalStrength signal;
    bool           known;      /* iwd already has credentials            */
    bool           connected;  /* currently the active network           */
} WifiNetwork;

/* ── lifecycle ──────────────────────────────────────────────────────────── */

/* One-time setup: enumerates adapters and selects the first with a station.
 * Returns 0 on success, <0 on error. */
int  wifi_init(void);

/* Release any resources acquired by wifi_init(). */
void wifi_cleanup(void);

/* ── adapter functions ──────────────────────────────────────────────────── */

/* Return the adapter list and write its length to *count_out.
 * Valid until the next wifi_init() or wifi_reload_adapters(). */
const WifiAdapter *wifi_get_adapters(int *count_out);

/* Re-enumerate adapters from iwd (e.g. after a power toggle). */
int wifi_reload_adapters(void);

/* Switch the active station to the adapter at index i.
 * Subsequent scan/connect/disconnect calls operate on that adapter.
 * Returns 0 on success, <0 on error. */
int wifi_set_active_adapter(int index);

/* Set the Powered property on the adapter at index i.
 * Returns 0 on success, <0 on error. */
int wifi_set_adapter_powered(int index, bool powered);

/* ── network functions ──────────────────────────────────────────────────── */

/* Trigger a scan and repopulate the network list.
 * Returns 0 on success, <0 on error. */
int wifi_scan(void);

/* Return the network list and write its length to *count_out.
 * Valid until the next wifi_scan(). */
const WifiNetwork *wifi_get_networks(int *count_out);

/* Connect to network at index i. For PSK networks iwd calls back into
 * the registered agent for the passphrase.
 * Returns 0 on success, -ECANCELED if user aborted, <0 on other error. */
int wifi_connect(int index);

/* Disconnect the current station. Returns 0 on success, <0 on error. */
int wifi_disconnect(void);

/* Forget the known network at index i, removing stored credentials.
 * Only valid if networks[i].known is true.
 * Returns 0 on success, <0 on error. */
int wifi_forget(int index);

/* Write a short description of the active adapter into buf, e.g.
 * "wlan0 [on]" or "wlan0 [off]". Safe to call when no station is active. */
void wifi_active_adapter_info(char *buf, int buf_len);

/* Convert a raw iwd RSSI value (100ths of dBm) to a SignalStrength bucket. */
SignalStrength wifi_rssi_to_strength(int16_t rssi);

#endif /* WIFI_H */

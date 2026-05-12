#ifndef WIFI_H
#define WIFI_H

#include <stdbool.h>
#include <systemd/sd-bus.h>

/*
 * wifi.h — iwd network operations
 *
 * Wraps net.connman.iwd Station and Network interfaces.
 * Builds and owns the list of visible networks; the UI layer
 * reads from that list but never writes to it directly.
 */

#define WIFI_SSID_MAX     33   /* 32-byte SSID + NUL                  */
#define WIFI_PATH_MAX    128   /* D-Bus object path                   */
#define WIFI_SECURITY_MAX 16   /* "psk", "8021x", "open", etc.        */
#define WIFI_MAX_NETWORKS 32   /* cap on displayed networks           */

/* Signal strength bucket used by the UI for the bar indicator. */
typedef enum {
    SIGNAL_NONE    = 0,   /* no signal / unknown     */
    SIGNAL_WEAK    = 1,   /* ▂░░░                    */
    SIGNAL_FAIR    = 2,   /* ▂▄░░                    */
    SIGNAL_GOOD    = 3,   /* ▂▄▆░                    */
    SIGNAL_STRONG  = 4    /* ▂▄▆█                    */
} SignalStrength;

/* One visible network as presented to the UI. */
typedef struct {
    char           ssid[WIFI_SSID_MAX];
    char           object_path[WIFI_PATH_MAX];      /* iwd D-Bus path for Connect()      */
    char           known_network_path[WIFI_PATH_MAX]; /* iwd KnownNetwork path for Forget() */
    char           security[WIFI_SECURITY_MAX];     /* "psk", "open", "8021x"            */
    SignalStrength signal;
    bool           known;       /* iwd already has credentials         */
    bool           connected;   /* currently the active network        */
} WifiNetwork;

/* Populate the network list from the iwd Station object.
 * Must call wifi_init() first. Returns 0 on success, <0 on error. */
int wifi_scan(void);

/* Return a pointer to the internal network array and write its
 * length to *count_out. The array is valid until the next wifi_scan(). */
const WifiNetwork *wifi_get_networks(int *count_out);

/* Connect to the network at index i in the current list.
 * For PSK networks iwd will call back into the registered agent for
 * the passphrase; this function blocks until connected or failed.
 * Returns 0 on success, <0 on error. */
int wifi_connect(int index);

/* Disconnect the current station. Returns 0 on success, <0 on error. */
int wifi_disconnect(void);

/* One-time setup: finds the first WiFi adapter via iwd's object manager
 * and stores its station path. Returns 0 on success, <0 on error.    */
int  wifi_init(void);

/* Release any resources acquired by wifi_init(). */
void wifi_cleanup(void);

/* Forget the known network at index i, removing stored credentials from iwd.
 * Only valid if the network's known field is true.
 * Returns 0 on success, <0 on error. */
int wifi_forget(int index);

/* Convert a raw RSSI value (dBm, typically -30 to -90) to a
 * SignalStrength bucket. */
SignalStrength wifi_rssi_to_strength(int16_t rssi);

#endif /* WIFI_H */

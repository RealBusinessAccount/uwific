#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <systemd/sd-bus.h>

#include "dbus.h"
#include "wifi.h"
#include "ui.h"

/*
 * wifi.c — iwd network operations
 *
 * iwd object hierarchy we care about:
 *
 *   /net/connman/iwd                        (ObjectManager)
 *     └─ /net/connman/iwd/0                 (Adapter)
 *          └─ /net/connman/iwd/0/3          (Device — net.connman.iwd.Device)
 *               ├─ net.connman.iwd.Station  (scan, list, connect, disconnect)
 *               └─ /net/connman/iwd/0/3/nnn (Network objects)
 *                    └─ net.connman.iwd.Network  (Name, Type, KnownNetwork)
 *
 * We discover the station path once at init time by walking GetManagedObjects.
 * Network objects are enumerated on each scan via GetOrderedNetworks(), which
 * returns (object_path, signal_strength) pairs in descending RSSI order.
 */

#define IWD_SERVICE          "net.connman.iwd"
#define IWD_ROOT_PATH        "/"
#define IWD_OBJECT_MANAGER   "org.freedesktop.DBus.ObjectManager"
#define IWD_STATION_IFACE    "net.connman.iwd.Station"
#define IWD_NETWORK_IFACE    "net.connman.iwd.Network"
#define IWD_DEVICE_IFACE     "net.connman.iwd.Device"

static char         g_station_path[WIFI_PATH_MAX] = {0};
static WifiNetwork  g_networks[WIFI_MAX_NETWORKS];
static int          g_network_count = 0;

/* ── init / cleanup ─────────────────────────────────────────────────────── */

/*
 * Walk iwd's GetManagedObjects response to find the first object that
 * exposes net.connman.iwd.Station, and store its path in g_station_path.
 *
 * The reply structure is:
 *   a{oa{sa{sv}}}
 *   array of:
 *     object_path ->
 *       array of:
 *         interface_name ->
 *           array of property_name -> variant
 *
 * We use sd_bus_message_skip to blast past the property dicts we don't
 * care about, and keep strict enter/exit symmetry so the parse position
 * never drifts between iterations.
 */
static int find_station_path(void)
{
    sd_bus_message *reply = NULL;
    int rc;

    rc = dbus_call_method(IWD_SERVICE,
                          IWD_ROOT_PATH,
                          IWD_OBJECT_MANAGER,
                          "GetManagedObjects",
                          &reply);
    if (rc < 0)
        return rc;

    /* Enter outer array of dict entries: a{oa{sa{sv}}} */
    rc = sd_bus_message_enter_container(reply, 'a', "{oa{sa{sv}}}");
    if (rc < 0) goto out;

    while ((rc = sd_bus_message_enter_container(reply, 'e', "oa{sa{sv}}")) > 0) {
        const char *obj_path = NULL;
        bool found_station = false;

        rc = sd_bus_message_read(reply, "o", &obj_path);
        if (rc < 0) goto out;

        /* Enter the interface dict: a{sa{sv}} */
        rc = sd_bus_message_enter_container(reply, 'a', "{sa{sv}}");
        if (rc < 0) goto out;

        while ((rc = sd_bus_message_enter_container(reply, 'e', "sa{sv}")) > 0) {
            const char *iface = NULL;

            rc = sd_bus_message_read(reply, "s", &iface);
            if (rc < 0) goto out;

            if (strcmp(iface, IWD_STATION_IFACE) == 0)
                found_station = true;

            /* Skip property dict regardless of whether we matched */
            rc = sd_bus_message_skip(reply, "a{sv}");
            if (rc < 0) goto out;

            rc = sd_bus_message_exit_container(reply); /* exit {sa{sv}} */
            if (rc < 0) goto out;
        }
        if (rc < 0) goto out;

        rc = sd_bus_message_exit_container(reply); /* exit a{sa{sv}} */
        if (rc < 0) goto out;

        rc = sd_bus_message_exit_container(reply); /* exit {oa{sa{sv}}} */
        if (rc < 0) goto out;

        if (found_station) {
            strncpy(g_station_path, obj_path, WIFI_PATH_MAX - 1);
            g_station_path[WIFI_PATH_MAX - 1] = '\0';
            break;
        }
    }
    if (rc < 0) goto out;

    /* If we broke early after finding the station, the message cursor is
     * still inside the object loop — sd-bus will refuse to exit the outer
     * array with EBUSY. That's fine; we're done with the message either way. */
    sd_bus_message_exit_container(reply); /* exit outer array, ignore rc */

    if (g_station_path[0] == '\0') {
        fprintf(stderr, "wifi_init: no iwd Station object found\n");
        rc = -ENODEV;
    } else {
        rc = 0;
    }

out:
    sd_bus_message_unref(reply);
    return rc;
}

int wifi_init(void)
{
    return find_station_path();
}

void wifi_cleanup(void)
{
    g_station_path[0] = '\0';
    g_network_count   = 0;
}

/* ── signal strength ────────────────────────────────────────────────────── */

SignalStrength wifi_rssi_to_strength(int16_t rssi)
{
    /* iwd reports signal strength in 100ths of a dBm (e.g. -6700 = -67 dBm).
     * Divide by 100 before applying the thresholds. */
    int dbm = rssi / 100;
    if (dbm >= -55) return SIGNAL_STRONG;
    if (dbm >= -65) return SIGNAL_GOOD;
    if (dbm >= -75) return SIGNAL_FAIR;
    if (dbm >  -90) return SIGNAL_WEAK;
    return SIGNAL_NONE;
}

/* ── network enumeration ────────────────────────────────────────────────── */

/*
 * Read the Name, Type, and KnownNetwork properties from a network object
 * and fill in a WifiNetwork.  RSSI and object_path are supplied by the
 * caller (they come from GetOrderedNetworks, not the network object itself).
 *
 * Returns 0 on success, <0 on error.
 */
static int read_network_properties(const char *net_path,
                                    int16_t     rssi,
                                    WifiNetwork *out)
{
    sd_bus_error    error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    sd_bus         *bus   = dbus_get_bus();
    int rc;

    memset(out, 0, sizeof(*out));
    strncpy(out->object_path, net_path, WIFI_PATH_MAX - 1);
    out->signal = wifi_rssi_to_strength(rssi);

    /* Read Name */
    char *name = NULL;
    rc = sd_bus_get_property_string(bus,
                                    IWD_SERVICE, net_path,
                                    IWD_NETWORK_IFACE, "Name",
                                    &error, &name);
    if (rc < 0) goto out;
    strncpy(out->ssid, name, WIFI_SSID_MAX - 1);
    free(name);
    sd_bus_error_free(&error);

    /* Read Type ("psk", "open", "8021x") */
    char *type = NULL;
    rc = sd_bus_get_property_string(bus,
                                    IWD_SERVICE, net_path,
                                    IWD_NETWORK_IFACE, "Type",
                                    &error, &type);
    if (rc < 0) goto out;
    strncpy(out->security, type, WIFI_SECURITY_MAX - 1);
    free(type);
    sd_bus_error_free(&error);

    /* Read Connected */
    int connected = 0;
    rc = sd_bus_get_property_trivial(bus,
                                     IWD_SERVICE, net_path,
                                     IWD_NETWORK_IFACE, "Connected",
                                     &error, 'b', &connected);
    if (rc < 0) goto out;
    out->connected = (bool)connected;
    sd_bus_error_free(&error);

    /*
     * Check KnownNetwork: this property holds an object path if iwd has
     * stored credentials, or is absent/empty if it doesn't.
     * We try to read it and treat any error as "not known".
     */
    rc = sd_bus_call_method(bus,
                            IWD_SERVICE, net_path,
                            "org.freedesktop.DBus.Properties", "Get",
                            &error, &reply,
                            "ss", IWD_NETWORK_IFACE, "KnownNetwork");
    if (rc >= 0) {
        const char *kn_path = NULL;
        rc = sd_bus_message_read(reply, "v", "o", &kn_path);
        if (rc >= 0 && kn_path && kn_path[0] != '\0') {
            out->known = true;
            strncpy(out->known_network_path, kn_path, WIFI_PATH_MAX - 1);
            out->known_network_path[WIFI_PATH_MAX - 1] = '\0';
        }
        sd_bus_message_unref(reply);
        reply = NULL;
    }
    sd_bus_error_free(&error);

    rc = 0; /* treat KnownNetwork read failure as non-fatal */

out:
    sd_bus_error_free(&error);
    if (reply) sd_bus_message_unref(reply);
    return rc;
}

/*
 * Call Station.GetOrderedNetworks() and populate g_networks[].
 *
 * Reply structure: a(on)
 *   array of (object_path, signal_strength_as_int16)
 */
int wifi_populate_networks(void)
{
    sd_bus_message *reply = NULL;
    int rc;

    rc = dbus_call_method(IWD_SERVICE,
                          g_station_path,
                          IWD_STATION_IFACE,
                          "GetOrderedNetworks",
                          &reply);
    if (rc < 0)
        return rc;

    g_network_count = 0;

    rc = sd_bus_message_enter_container(reply, 'a', "(on)");
    if (rc < 0) goto out;

    while ((rc = sd_bus_message_enter_container(reply, 'r', "on")) > 0) {
        const char *net_path = NULL;
        int16_t     rssi     = 0;

        rc = sd_bus_message_read(reply, "on", &net_path, &rssi);
        if (rc < 0) {
            sd_bus_message_exit_container(reply);
            goto out;
        }

        rc = sd_bus_message_exit_container(reply); /* exit (on) */
        if (rc < 0) goto out;

        if (g_network_count >= WIFI_MAX_NETWORKS)
            continue; /* list full — skip the rest */

        /* read_network_properties is best-effort; skip entries that fail */
        if (read_network_properties(net_path, rssi,
                                    &g_networks[g_network_count]) == 0)
            g_network_count++;
    }
    if (rc < 0) goto out;

    rc = sd_bus_message_exit_container(reply); /* exit outer array */

out:
    sd_bus_message_unref(reply);
    return (rc < 0) ? rc : 0;
}

/* ── scan ───────────────────────────────────────────────────────────────── */

int wifi_scan(void)
{
    int rc;

    if (g_station_path[0] == '\0')
        return -ENODEV;

    /* Trigger a scan and wait for it to complete.
     * iwd's Scan() blocks until the scan is done. */
    rc = dbus_call_method(IWD_SERVICE,
                          g_station_path,
                          IWD_STATION_IFACE,
                          "Scan",
                          NULL);
    if (rc < 0)
        return rc;

    return wifi_populate_networks();
}

/* ── public accessors ───────────────────────────────────────────────────── */

const WifiNetwork *wifi_get_networks(int *count_out)
{
    *count_out = g_network_count;
    return g_networks;
}

/* ── connect / disconnect ───────────────────────────────────────────────── */

/*
 * wifi_connect — send Connect() and pump the D-Bus event loop until done.
 *
 * The naive approach of calling dbus_call_method() (which uses sd_bus_call,
 * a blocking call) deadlocks on PSK networks: iwd needs to call back into
 * our agent's RequestPassphrase() method *during* the Connect() call, but
 * while we're blocked inside sd_bus_call we never process incoming messages,
 * so the agent callback never fires and iwd times out.
 *
 * The fix: send the method call as a fire-and-forget with a reply callback,
 * then spin on sd_bus_process() so both incoming agent calls and the eventual
 * Connect() reply are dispatched. We also redraw the spinner on each iteration
 * so the UI stays alive during the wait.
 */

typedef struct {
    int  rc;
    bool done;
} ConnectResult;

static int connect_reply_handler(sd_bus_message *msg,
                                  void           *userdata,
                                  sd_bus_error   *ret_error)
{
    (void)ret_error;
    ConnectResult *result = userdata;

    if (sd_bus_message_is_method_error(msg, NULL)) {
        const sd_bus_error *err = sd_bus_message_get_error(msg);
        /* Translate the iwd error name to a sensible errno */
        if (err && strstr(err->name, "Canceled"))
            result->rc = -ECANCELED;
        else
            result->rc = -EIO;
    } else {
        result->rc = 0;
    }
    result->done = true;
    return 0;
}

int wifi_connect(int index)
{
    if (index < 0 || index >= g_network_count)
        return -EINVAL;

    if (g_networks[index].connected)
        return 0;

    sd_bus        *bus = dbus_get_bus();
    ConnectResult  result = { .rc = 0, .done = false };
    int rc;

    /* Send Connect() asynchronously — reply handled by connect_reply_handler. */
    rc = sd_bus_call_method_async(bus,
                                  NULL,
                                  IWD_SERVICE,
                                  g_networks[index].object_path,
                                  IWD_NETWORK_IFACE,
                                  "Connect",
                                  connect_reply_handler,
                                  &result,
                                  NULL);
    if (rc < 0)
        return rc;

    /*
     * Pump the bus until the reply (or an error) arrives.
     * sd_bus_process dispatches one pending message per call — incoming
     * agent RequestPassphrase calls are handled here, which unblocks iwd
     * so it can complete the connection and send us the Connect() reply.
     *
     * We pass a short timeout to sd_bus_wait so the loop stays responsive
     * for UI redraws without burning the CPU.
     */
    while (!result.done) {
        rc = sd_bus_process(bus, NULL);
        if (rc < 0)
            return rc;
        if (rc == 0) {
            /* No messages ready — wait briefly then redraw spinner. */
            sd_bus_wait(bus, 100 * 1000); /* 100 ms in microseconds */
            ui_draw();
        }
    }

    return result.rc;
}

int wifi_forget(int index)
{
    if (index < 0 || index >= g_network_count)
        return -EINVAL;

    if (!g_networks[index].known)
        return -EINVAL;

    return dbus_call_method(IWD_SERVICE,
                            g_networks[index].known_network_path,
                            "net.connman.iwd.KnownNetwork",
                            "Forget",
                            NULL);
}

int wifi_disconnect(void)
{
    if (g_station_path[0] == '\0')
        return -ENODEV;

    return dbus_call_method(IWD_SERVICE,
                            g_station_path,
                            IWD_STATION_IFACE,
                            "Disconnect",
                            NULL);
}

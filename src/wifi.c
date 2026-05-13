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
 *     └─ /net/connman/iwd/0                 (Adapter — net.connman.iwd.Adapter)
 *          └─ /net/connman/iwd/0/4          (Device — net.connman.iwd.Device)
 *               ├─ net.connman.iwd.Station  (scan, list, connect, disconnect)
 *               └─ /net/connman/iwd/0/4/nnn (Network objects)
 *                    └─ net.connman.iwd.Network  (Name, Type, KnownNetwork)
 *
 * We enumerate all adapters and stations once at init time by walking
 * GetManagedObjects. The first station found becomes the active one.
 * Network objects are enumerated on each scan via GetOrderedNetworks().
 */

#define IWD_SERVICE          "net.connman.iwd"
#define IWD_ROOT_PATH        "/"
#define IWD_OBJECT_MANAGER   "org.freedesktop.DBus.ObjectManager"
#define IWD_ADAPTER_IFACE    "net.connman.iwd.Adapter"
#define IWD_STATION_IFACE    "net.connman.iwd.Station"
#define IWD_NETWORK_IFACE    "net.connman.iwd.Network"
#define IWD_DEVICE_IFACE     "net.connman.iwd.Device"

static char        g_station_path[WIFI_PATH_MAX] = {0};
static WifiNetwork g_networks[WIFI_MAX_NETWORKS];
static int         g_network_count = 0;

static WifiAdapter g_adapters[WIFI_MAX_ADAPTERS];
static int         g_adapter_count = 0;

/* ── enumerate helpers ──────────────────────────────────────────────────── */

/*
 * Wrapper around dbus_call_method that suppresses error output.
 * Used during adapter reload where a transient bus disconnect is expected.
 */
static int dbus_call_method_silent(const char      *dest,
                                    const char      *path,
                                    const char      *iface,
                                    const char      *method,
                                    sd_bus_message **reply_out)
{
    sd_bus_error    error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int rc;

    rc = sd_bus_call_method(dbus_get_bus(), dest, path, iface, method,
                            &error, &reply, NULL);
    sd_bus_error_free(&error);
    if (rc < 0) return rc;

    if (reply_out) *reply_out = reply;
    else sd_bus_message_unref(reply);
    return 0;
}

/*
 * Walk GetManagedObjects and build g_adapters[].
 *
 * For each object we record whether it exposes net.connman.iwd.Adapter
 * and/or net.connman.iwd.Station, then do a second pass to pair them up:
 * a Station lives under its Adapter in the object path hierarchy, so
 * /net/connman/iwd/0/4 is the station for adapter /net/connman/iwd/0.
 *
 * After enumeration we read the Powered and Name properties for each
 * adapter and set g_station_path to the first powered (or any) station.
 */

#define MAX_OBJECTS 64

typedef struct {
    char path[WIFI_PATH_MAX];
    bool has_adapter;
    bool has_station;
} ObjectEntry;

static int enumerate_adapters(void)
{
    sd_bus_message *reply = NULL;
    int rc;

    rc = dbus_call_method_silent(IWD_SERVICE, IWD_ROOT_PATH,
                                  IWD_OBJECT_MANAGER, "GetManagedObjects", &reply);
    if (rc < 0)
        return rc;

    ObjectEntry objects[MAX_OBJECTS];
    int         obj_count = 0;

    rc = sd_bus_message_enter_container(reply, 'a', "{oa{sa{sv}}}");
    if (rc < 0) goto out;

    while ((rc = sd_bus_message_enter_container(reply, 'e', "oa{sa{sv}}")) > 0) {
        const char *obj_path = NULL;
        bool has_adapter = false, has_station = false;

        rc = sd_bus_message_read(reply, "o", &obj_path);
        if (rc < 0) goto out;

        rc = sd_bus_message_enter_container(reply, 'a', "{sa{sv}}");
        if (rc < 0) goto out;

        while ((rc = sd_bus_message_enter_container(reply, 'e', "sa{sv}")) > 0) {
            const char *iface = NULL;
            rc = sd_bus_message_read(reply, "s", &iface);
            if (rc < 0) goto out;

            if (strcmp(iface, IWD_ADAPTER_IFACE) == 0) has_adapter = true;
            if (strcmp(iface, IWD_STATION_IFACE)  == 0) has_station = true;

            rc = sd_bus_message_skip(reply, "a{sv}");
            if (rc < 0) goto out;
            rc = sd_bus_message_exit_container(reply);
            if (rc < 0) goto out;
        }
        if (rc < 0) goto out;

        rc = sd_bus_message_exit_container(reply); /* exit a{sa{sv}} */
        if (rc < 0) goto out;
        rc = sd_bus_message_exit_container(reply); /* exit {oa{sa{sv}}} */
        if (rc < 0) goto out;

        if ((has_adapter || has_station) && obj_count < MAX_OBJECTS) {
            strncpy(objects[obj_count].path, obj_path, WIFI_PATH_MAX - 1);
            objects[obj_count].has_adapter = has_adapter;
            objects[obj_count].has_station = has_station;
            obj_count++;
        }
    }
    if (rc < 0) goto out;

    /* ignore rc from outer array exit — may be EBUSY if we broke early */
    sd_bus_message_exit_container(reply);

    /* ── pair adapters with their stations ── */
    g_adapter_count = 0;
    for (int i = 0; i < obj_count && g_adapter_count < WIFI_MAX_ADAPTERS; i++) {
        if (!objects[i].has_adapter)
            continue;

        WifiAdapter *a = &g_adapters[g_adapter_count];
        memset(a, 0, sizeof(*a));
        strncpy(a->object_path, objects[i].path, WIFI_PATH_MAX - 1);

        /* Find the station whose path starts with this adapter's path */
        for (int j = 0; j < obj_count; j++) {
            if (!objects[j].has_station)
                continue;
            if (strncmp(objects[j].path, a->object_path,
                        strlen(a->object_path)) == 0) {
                strncpy(a->station_path, objects[j].path, WIFI_PATH_MAX - 1);
                break;
            }
        }

        /* Read Powered and Name (phy name) from the Adapter object */
        bool powered = false;
        if (dbus_get_property_bool(IWD_SERVICE, a->object_path,
                                   IWD_ADAPTER_IFACE, "Powered",
                                   &powered) == 0)
            a->powered = powered;

        char *name = NULL;
        if (dbus_get_property_string(IWD_SERVICE, a->object_path,
                                     IWD_ADAPTER_IFACE, "Name",
                                     &name) == 0 && name) {
            strncpy(a->name, name, WIFI_ADAPTER_NAME_MAX - 1);
            free(name);
        } else {
            const char *slash = strrchr(a->object_path, '/');
            strncpy(a->name, slash ? slash + 1 : a->object_path,
                    WIFI_ADAPTER_NAME_MAX - 1);
        }

        /* Read interface name (e.g. "wlan0") from the Device object,
         * which lives at the same path as the station. Best-effort. */
        if (a->station_path[0] != '\0') {
            char *ifname = NULL;
            if (dbus_get_property_string(IWD_SERVICE, a->station_path,
                                         IWD_DEVICE_IFACE, "Name",
                                         &ifname) == 0 && ifname) {
                strncpy(a->ifname, ifname, WIFI_ADAPTER_NAME_MAX - 1);
                free(ifname);
            }
        }
        /* Fall back to phy name if interface name unavailable */
        if (a->ifname[0] == '\0')
            strncpy(a->ifname, a->name, WIFI_ADAPTER_NAME_MAX - 1);

        g_adapter_count++;
    }

    if (g_adapter_count == 0) {
        fprintf(stderr, "wifi_init: no iwd Adapter objects found\n");
        rc = -ENODEV;
        goto out;
    }

    /* Try to find a station to make active. If none exists (all adapters
     * powered off), that's fine — the UI will show the powered-off state
     * and the user can power one on from the options menu. */
    rc = 0;
    for (int i = 0; i < g_adapter_count; i++) {
        if (g_adapters[i].station_path[0] != '\0') {
            g_adapters[i].active = true;
            strncpy(g_station_path, g_adapters[i].station_path,
                    WIFI_PATH_MAX - 1);
            break;
        }
    }
    /* Mark first adapter active for UI purposes even if it has no station */
    if (g_adapter_count > 0) {
        bool any_active = false;
        for (int i = 0; i < g_adapter_count; i++)
            if (g_adapters[i].active) { any_active = true; break; }
        if (!any_active)
            g_adapters[0].active = true;
    }

out:
    sd_bus_message_unref(reply);
    return rc;
}

int wifi_init(void)
{
    return enumerate_adapters();
}

void wifi_cleanup(void)
{
    g_station_path[0] = '\0';
    g_network_count   = 0;
    g_adapter_count   = 0;
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

/* ── adapter functions ──────────────────────────────────────────────────── */

void wifi_active_adapter_info(char *buf, int buf_len)
{
    for (int i = 0; i < g_adapter_count; i++) {
        if (g_adapters[i].active) {
            snprintf(buf, buf_len, "%s on %s - [%s]",
                     g_adapters[i].ifname,
                     g_adapters[i].name,
                     g_adapters[i].powered ? "on" : "off");
            return;
        }
    }
    snprintf(buf, buf_len, "no adapter");
}


const WifiAdapter *wifi_get_adapters(int *count_out)
{
    *count_out = g_adapter_count;
    return g_adapters;
}

int wifi_reload_adapters(void)
{
    /* Stash the current cache so we can restore it if enumeration fails.
     * This matters when iwd briefly disconnects after a power toggle —
     * a failed reload should leave the UI with the last-known state rather
     * than an empty adapter list. */
    WifiAdapter saved[WIFI_MAX_ADAPTERS];
    int         saved_count = g_adapter_count;
    memcpy(saved, g_adapters, sizeof(WifiAdapter) * g_adapter_count);

    g_adapter_count = 0;
    g_network_count = 0;

    int rc = enumerate_adapters();
    if (rc < 0) {
        /* Restore the stale cache */
        memcpy(g_adapters, saved, sizeof(WifiAdapter) * saved_count);
        g_adapter_count = saved_count;
    }
    return rc;
}

int wifi_set_active_adapter(int index)
{
    if (index < 0 || index >= g_adapter_count)
        return -EINVAL;

    if (g_adapters[index].station_path[0] == '\0')
        return -ENODEV;

    for (int i = 0; i < g_adapter_count; i++)
        g_adapters[i].active = (i == index);

    strncpy(g_station_path, g_adapters[index].station_path, WIFI_PATH_MAX - 1);
    g_network_count = 0;
    return 0;
}

int wifi_set_adapter_powered(int index, bool powered)
{
    if (index < 0 || index >= g_adapter_count)
        return -EINVAL;

    int rc = dbus_set_property_bool(IWD_SERVICE,
                                    g_adapters[index].object_path,
                                    IWD_ADAPTER_IFACE,
                                    "Powered",
                                    powered);
    if (rc == 0)
        g_adapters[index].powered = powered;

    return rc;
}

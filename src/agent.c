#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <systemd/sd-bus.h>

#include "dbus.h"
#include "agent.h"

/*
 * agent.c — iwd passphrase agent
 *
 * iwd uses an agent model for credential prompting: rather than the
 * caller pushing a passphrase into Connect(), iwd calls back into a
 * D-Bus object we register.  This keeps the connect flow clean and
 * means iwd controls the timing of when credentials are requested.
 *
 * Protocol:
 *   1. We publish an object at AGENT_PATH implementing
 *      net.connman.iwd.Agent.
 *   2. We call net.connman.iwd.AgentManager.RegisterAgent(AGENT_PATH).
 *   3. When Connect() needs a passphrase, iwd calls our
 *      RequestPassphrase(network_path) method.
 *   4. We invoke the UI callback, collect the passphrase, and return
 *      it in the D-Bus reply.
 *   5. On program exit we call UnregisterAgent(AGENT_PATH).
 *
 * If the user cancels we return the net.connman.iwd.Agent.Error.Canceled
 * error to iwd, which causes Connect() to fail gracefully.
 */

#define IWD_SERVICE          "net.connman.iwd"
#define IWD_AGENT_MGR_PATH   "/net/connman/iwd"
#define IWD_AGENT_MGR_IFACE  "net.connman.iwd.AgentManager"
#define IWD_AGENT_IFACE      "net.connman.iwd.Agent"
#define IWD_NETWORK_IFACE    "net.connman.iwd.Network"

#define PASSPHRASE_MAX 64

static AgentPassphraseCallback g_passphrase_cb = NULL;
static sd_bus_slot            *g_agent_slot    = NULL;
static bool                    g_registered    = false;

/* ── callback registration ──────────────────────────────────────────────── */

void agent_set_passphrase_callback(AgentPassphraseCallback cb)
{
    g_passphrase_cb = cb;
}

/* ── D-Bus method handlers ──────────────────────────────────────────────── */

/*
 * net.connman.iwd.Agent.RequestPassphrase(network_path) -> passphrase
 *
 * iwd calls this when it needs credentials for a PSK network.
 * We read the network's Name property to show a friendly prompt,
 * invoke the UI callback, then either reply with the passphrase or
 * return the Canceled error.
 */
static int handle_request_passphrase(sd_bus_message *msg,
                                     void           *userdata,
                                     sd_bus_error   *ret_error)
{
    (void)userdata;

    const char *net_path = NULL;
    int rc;

    rc = sd_bus_message_read(msg, "o", &net_path);
    if (rc < 0) {
        sd_bus_error_set_errno(ret_error, -rc);
        return rc;
    }

    /* Look up the SSID for a friendlier prompt. Best-effort — if this
     * fails we fall back to showing the raw object path. */
    char *ssid = NULL;
    sd_bus_error prop_error = SD_BUS_ERROR_NULL;
    rc = sd_bus_get_property_string(dbus_get_bus(),
                                    IWD_SERVICE, net_path,
                                    IWD_NETWORK_IFACE, "Name",
                                    &prop_error, &ssid);
    const char *display_name = (rc >= 0 && ssid) ? ssid : net_path;
    sd_bus_error_free(&prop_error);

    char passphrase[PASSPHRASE_MAX] = {0};

    if (!g_passphrase_cb) {
        free(ssid);
        sd_bus_error_set(ret_error,
                         "net.connman.iwd.Agent.Error.Canceled",
                         "No passphrase callback registered");
        return -ECANCELED;
    }

    rc = g_passphrase_cb(display_name, passphrase, sizeof(passphrase));
    free(ssid);

    if (rc == -ECANCELED) {
        sd_bus_error_set(ret_error,
                         "net.connman.iwd.Agent.Error.Canceled",
                         "User canceled passphrase entry");
        return -ECANCELED;
    }

    if (rc < 0) {
        sd_bus_error_set_errno(ret_error, -rc);
        return rc;
    }

    return sd_bus_reply_method_return(msg, "s", passphrase);
}

/*
 * net.connman.iwd.Agent.Release()
 *
 * iwd calls this when it is shutting down or unregistering agents.
 * We acknowledge it and mark ourselves as unregistered so cleanup
 * doesn't try to call UnregisterAgent on a dead service.
 */
static int handle_release(sd_bus_message *msg,
                           void           *userdata,
                           sd_bus_error   *ret_error)
{
    (void)userdata;
    (void)ret_error;

    g_registered = false;
    return sd_bus_reply_method_return(msg, "");
}

/*
 * net.connman.iwd.Agent.Cancel(reason)
 *
 * iwd calls this to abort an in-progress RequestPassphrase — e.g. if
 * the user connected from another client while we were prompting.
 * For now we just acknowledge it; the UI prompt will stay open until
 * the user acts, which is acceptable for a single-user device.
 */
static int handle_cancel(sd_bus_message *msg,
                          void           *userdata,
                          sd_bus_error   *ret_error)
{
    (void)userdata;
    (void)ret_error;

    const char *reason = NULL;
    sd_bus_message_read(msg, "s", &reason);
    fprintf(stderr, "agent: iwd canceled request (%s)\n",
            reason ? reason : "unknown");

    return sd_bus_reply_method_return(msg, "");
}

/* ── vtable ─────────────────────────────────────────────────────────────── */

static const sd_bus_vtable agent_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("RequestPassphrase", "o",  "s", handle_request_passphrase, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Release",           "",   "",  handle_release,            SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Cancel",            "s",  "",  handle_cancel,             SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

/* ── lifecycle ──────────────────────────────────────────────────────────── */

int agent_init(void)
{
    sd_bus      *bus = dbus_get_bus();
    sd_bus_error  error = SD_BUS_ERROR_NULL;
    int rc;

    /* Publish our agent object on the bus. */
    rc = sd_bus_add_object_vtable(bus,
                                  &g_agent_slot,
                                  AGENT_PATH,
                                  IWD_AGENT_IFACE,
                                  agent_vtable,
                                  NULL);
    if (rc < 0) {
        fprintf(stderr, "agent_init: failed to register object: %s\n",
                strerror(-rc));
        return rc;
    }

    /*
     * Tell iwd where our agent lives.
     * RegisterAgent takes a single object-path argument, so we can't
     * use the no-args dbus_call_method helper here.
     */
    rc = sd_bus_call_method(bus,
                            IWD_SERVICE,
                            IWD_AGENT_MGR_PATH,
                            IWD_AGENT_MGR_IFACE,
                            "RegisterAgent",
                            &error,
                            NULL,          /* no reply needed */
                            "o", AGENT_PATH);
    if (rc < 0) {
        fprintf(stderr, "agent_init: RegisterAgent failed: %s\n",
                error.message ? error.message : strerror(-rc));
        sd_bus_error_free(&error);
        sd_bus_slot_unref(g_agent_slot);
        g_agent_slot = NULL;
        return rc;
    }

    sd_bus_error_free(&error);
    g_registered = true;
    return 0;
}

void agent_cleanup(void)
{
    if (g_registered) {
        sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus_call_method(dbus_get_bus(),
                           IWD_SERVICE,
                           IWD_AGENT_MGR_PATH,
                           IWD_AGENT_MGR_IFACE,
                           "UnregisterAgent",
                           &error,
                           NULL,
                           "o", AGENT_PATH);
        sd_bus_error_free(&error);
        g_registered = false;
    }

    if (g_agent_slot) {
        sd_bus_slot_unref(g_agent_slot);
        g_agent_slot = NULL;
    }
}

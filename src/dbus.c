#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <systemd/sd-bus.h>

#include "dbus.h"

/*
 * dbus.c — low-level sd-bus wrapper
 *
 * Owns the single system-bus connection used by the whole program.
 * All iwd interaction goes through the helpers here so that the
 * higher-level modules never need to touch sd-bus plumbing directly.
 */

static sd_bus *g_bus = NULL;

/* ── lifecycle ─────────────────────────────────────────────────────────── */

int dbus_init(void)
{
    int rc = sd_bus_open_system(&g_bus);
    if (rc < 0) {
        fprintf(stderr, "dbus_init: sd_bus_open_system failed: %s\n",
                strerror(-rc));
        return rc;
    }
    return 0;
}

void dbus_cleanup(void)
{
    if (g_bus) {
        sd_bus_unref(g_bus);
        g_bus = NULL;
    }
}

sd_bus *dbus_get_bus(void)
{
    return g_bus;
}

/* ── method call helper ─────────────────────────────────────────────────── */

int dbus_call_method(const char      *dest,
                     const char      *path,
                     const char      *iface,
                     const char      *method,
                     sd_bus_message **reply_out)
{
    sd_bus_error    error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int rc;

    rc = sd_bus_call_method(g_bus,
                            dest,
                            path,
                            iface,
                            method,
                            &error,
                            &reply,
                            /* no in-args */ NULL);
    if (rc < 0) {
        fprintf(stderr, "dbus_call_method: %s.%s on %s failed: %s\n",
                iface, method, path,
                error.message ? error.message : strerror(-rc));
        sd_bus_error_free(&error);
        return rc;
    }

    sd_bus_error_free(&error);

    if (reply_out)
        *reply_out = reply;   /* caller takes ownership */
    else
        sd_bus_message_unref(reply);

    return 0;
}

/* ── property getter ────────────────────────────────────────────────────── */

int dbus_get_property_string(const char *dest,
                              const char *path,
                              const char *iface,
                              const char *property,
                              char      **value_out)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    char        *value = NULL;
    int          rc;

    rc = sd_bus_get_property_string(g_bus,
                                    dest,
                                    path,
                                    iface,
                                    property,
                                    &error,
                                    &value);
    if (rc < 0) {
        fprintf(stderr,
                "dbus_get_property_string: %s.%s on %s failed: %s\n",
                iface, property, path,
                error.message ? error.message : strerror(-rc));
        sd_bus_error_free(&error);
        return rc;
    }

    sd_bus_error_free(&error);
    *value_out = value;   /* caller must free() */
    return 0;
}

/* ── boolean property get/set ───────────────────────────────────────────── */

int dbus_get_property_bool(const char *dest,
                            const char *path,
                            const char *iface,
                            const char *property,
                            bool       *value_out)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int          val   = 0;
    int          rc;

    rc = sd_bus_get_property_trivial(g_bus,
                                     dest, path, iface, property,
                                     &error, 'b', &val);
    if (rc < 0) {
        fprintf(stderr,
                "dbus_get_property_bool: %s.%s on %s failed: %s\n",
                iface, property, path,
                error.message ? error.message : strerror(-rc));
        sd_bus_error_free(&error);
        return rc;
    }

    sd_bus_error_free(&error);
    *value_out = (bool)val;
    return 0;
}

int dbus_set_property_bool(const char *dest,
                            const char *path,
                            const char *iface,
                            const char *property,
                            bool        value)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int          rc;

    rc = sd_bus_set_property(g_bus,
                             dest, path, iface, property,
                             &error, "b", (int)value);
    if (rc < 0) {
        fprintf(stderr,
                "dbus_set_property_bool: %s.%s on %s failed: %s\n",
                iface, property, path,
                error.message ? error.message : strerror(-rc));
        sd_bus_error_free(&error);
        return rc;
    }

    sd_bus_error_free(&error);
    return 0;
}

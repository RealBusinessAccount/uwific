#ifndef DBUS_H
#define DBUS_H

#include <systemd/sd-bus.h>

/*
 * dbus.h — low-level sd-bus wrapper
 *
 * Owns the single bus connection used by the whole program.
 * All other modules call through these helpers rather than
 * touching sd-bus directly.
 */

/* Initialise the system bus connection. Returns 0 on success, <0 on error. */
int  dbus_init(void);

/* Release the bus connection. Safe to call if dbus_init() was never called. */
void dbus_cleanup(void);

/* Return the raw sd_bus* for modules that need to pass it to sd-bus APIs
 * (e.g. agent registration). Never store this pointer across event loop
 * iterations without holding a reference. */
sd_bus *dbus_get_bus(void);

/*
 * Convenience wrapper: call a D-Bus method with no in-arguments and expect
 * a reply. Caller owns the returned sd_bus_message* and must unref it.
 *
 *   dest      e.g. "net.connman.iwd"
 *   path      object path
 *   iface     interface name
 *   method    method name
 *   reply_out receives the reply message (may be NULL if reply not needed)
 *
 * Returns 0 on success, <0 on sd-bus error.
 */
int dbus_call_method(const char *dest,
                     const char *path,
                     const char *iface,
                     const char *method,
                     sd_bus_message **reply_out);

/*
 * Read a single object-path property from a D-Bus interface.
 * Caller must free() the returned string.
 */
int dbus_get_property_string(const char *dest,
                              const char *path,
                              const char *iface,
                              const char *property,
                              char **value_out);

#endif /* DBUS_H */

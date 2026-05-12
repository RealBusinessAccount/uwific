#ifndef AGENT_H
#define AGENT_H

#include <systemd/sd-bus.h>

/*
 * agent.h — iwd passphrase agent
 *
 * Registers a D-Bus object that iwd calls back into when it needs
 * credentials during a Connect().  The agent blocks in a ncurses
 * password prompt and returns the passphrase to iwd via the reply.
 *
 * Lifecycle:
 *   1. Call agent_init() after dbus_init() and ui_init().
 *   2. The agent stays registered for the lifetime of the program.
 *   3. Call agent_cleanup() before exit.
 *
 * The agent object is published at AGENT_PATH on the session-local
 * bus name; iwd is told about it via AgentManager.RegisterAgent().
 */

#define AGENT_PATH "/net/connman/calwifi/Agent"

/*
 * Register the agent object on the bus and tell iwd about it.
 * Returns 0 on success, <0 on error.
 */
int  agent_init(void);

/*
 * Unregister from iwd and remove the bus object.
 * Safe to call even if agent_init() failed.
 */
void agent_cleanup(void);

/*
 * Callback invoked by the agent's RequestPassphrase handler.
 * The agent calls this to hand off the password prompt to the UI layer,
 * then passes the result back in the D-Bus reply.
 *
 * ssid        — the network name, for display in the prompt
 * buf         — caller-supplied buffer to receive the passphrase
 * buf_len     — size of buf
 *
 * Returns 0 if the user supplied a passphrase, -ECANCELED if they
 * pressed Esc/q to abort.
 */
typedef int (*AgentPassphraseCallback)(const char *ssid,
                                       char       *buf,
                                       int         buf_len);

/*
 * Register the UI callback that agent.c will invoke when iwd asks
 * for a passphrase.  Must be called before agent_init().
 */
void agent_set_passphrase_callback(AgentPassphraseCallback cb);

#endif /* AGENT_H */

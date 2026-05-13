# uwific - Micro WiFi Client

A TUI WiFi manager designed for small-screen Linux devices. Talks directly to
[iwd](https://iwd.wiki.kernel.org/) via D-Bus, so it behaves as a proper
iwd client and benefits from iwd's built-in credential persistence.
Designed specifically for [Calculinux](https://calculinux.org) running on modified PicoCalc devices,
but should work just as well on any device with a TTY that uses iwd to manage
wireless connections.

## Features

- Scans for and lists visible networks with signal strength indicators
- Connects to open and PSK-secured networks, prompting for a password when needed
- Disconnects from the current network
- Forgets known networks (removes stored credentials from iwd)
- Adapts to whatever terminal size is available
- Credentials are persisted by iwd automatically — known networks reconnect
  on subsequent boots without needing to run uwific at all
- Support for confiugration of multiple adapters and interfaces.

## Requirements

- `iwd` running and managing the WiFi adapter
- `libsystemd` (for sd-bus)
- `ncurses`
- A C11 compiler and `pkg-config`

On Calculinux, install build dependencies with:

```
opkg install systemd-dev libncurses-dev
```

## Build

```
make
```

To install to `/usr/local/bin`:

```
sudo make install
```

To uninstall:

```
sudo make uninstall
````

## Usage

```
sudo uwific
```

> **Note:** uwific communicates with iwd over the system D-Bus, which
> requires root privileges.

## Controls

| Key        | Action                                      |
|------------|---------------------------------------------|
| Up / k     | Move cursor up                              |
| Down / j   | Move cursor down                            |
| Enter      | Connect to selected network                 |
| D          | Disconnect from current network             |
| F          | Forget selected network (Y/N confirmation)  |
| R          | Rescan for networks                         |
| O          | Open adapter options menu                   |
| P          | Toggle adapter power (in options menu)      |
| Q          | Quit                                        |

## Network list indicators

| Indicator | Meaning                              |
|-----------|--------------------------------------|
| `####`    | Strong signal                        |
| `###+`    | Good signal                          |
| `##++`    | Fair signal                          |
| `#+++`    | Weak signal                          |
| `++++`    | Very weak / no signal                |
| `[*]`     | Currently connected                  |
| `[k]`     | Known network (credentials stored)   |

## Architecture

| File        | Responsibility                                    |
|-------------|---------------------------------------------------|
| `main.c`    | Entry point, event loop                           |
| `dbus.c/h`  | sd-bus connection, low-level helpers              |
| `wifi.c/h`  | iwd Station / Network interface, scan, connect    |
| `agent.c/h` | D-Bus agent for passphrase callbacks from iwd     |
| `ui.c/h`    | ncurses rendering, input, dialogs                 |

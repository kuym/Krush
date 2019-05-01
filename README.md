# Krush

Krush is a lightweight, TLS-encrypted remote access client written in C. It’s designed to provide a secure way to interact with an interactive shell on a remote server, effectively acting as a "reverse" connection that you control through an encrypted tunnel.

Unlike traditional reverse shells which can be noisy and insecure, Krush leverages OpenSSL for robust encryption and handles the complexities of PTY (Pseudo-terminal) management and window resizing, making the experience feel much more like a standard SSH session.

Not implement yet: multiplexing multiple channels of terminal (PTY) processes, sockets, ports, files and so on over the same reverse channel.  Some inactive parts of the implementation are in the code.

## Features

- **Fully Encrypted & Authenticated**: Uses TLS via OpenSSL with **Certificate Pinning**. It trusts either a user-provided CA (`--cert`) or a specific certificate hardcoded into the binary (via the `-a` preset), effectively preventing Man-in-the-Middle (MITM) attacks.
- **Interactive Shell Support**: Seamlessly handles interactive programs (like `top`, `vim`, or `nano`) by managing PTYs and terminal resizing.
- **Automatic Identification**: Supports identifying the connected device using a provided identity file, helping you keep track of which machine is connecting. This identity is even embedded into your shell's `PS1` prompt!
- **Resilient Connection**:
    - **Auto-Reconnect**: If the network drops, DNS fails, or the link breaks, Krush automatically retries connecting.
    - **Auto-Restart**: If the shell process exits (e_g., you type `exit`), Krush immediately relaunches it.
- **Lightweight & Single File**: The core logic resides in a single C file, making it easy to audit and compile anywhere.

## Prerequisites

To build Krush, you will need:

- A C11 compiler (like `gcc` or `clang`)
- OpenSSL development headers (`libssl-dev` on Debian/Ubuntu, or `openssl` via Homebrew on macOS)
- `libresolv` (usually present on most Unix-like systems)

## Building from Source

Building is straightforward with the provided `Makefile`. Just run `make`.  This will generate an executable named `krush`.

### Note for macOS Users (Homebrew OpenSSL)
If you are using Homebrew, OpenSSL might not be in your default include/library path. You can compile it manually by pointing to your OpenSSl installation:

```bash
SSL=/opt/homebrew/opt/openssl@3 cc -std=c11 -O2 -Wall -Wextra \
   -I"$SSL/include" -L"$SSL/lib" \
   krush.c -o krush -lssl -lcrypto -lresolv
```

## Usage

The client requires a remote host, a port, and a trusted certificate to establish the TLS connection.

### Basic Example

If you have a server listening on `example.com:8181` and you want to use a specific CA certificate for verification:

```bash
./krush --remote example.com --port 8181 --cert ./server-ca.pem
```

### Configuration Options

| Flag | Long Name | Arg | Description |
| :--- | :--- | :--- | :--- |
| `-r` | `--remote` | hostname | **(Required)** The FQDN of the server to connect to. |
| `-p` | `--port` | number | **(Required)** The server TCP port. |
| `-c` | `--cert` | path | **(Required*)** Path to the CA certificate for verification. |
| `-s` | `--shell` | path | Path of the shell command to invoke (default: `/bin/bash`). |
| `-d` | `--cwd` | path | Working directory for the session. |
| `-i` | `--ident` | path | Path to a file containing a unique identity string. |
| `-a` | N/A | — | **Preset Mode**: Connects using hardcoded host/port and built-in cert. |

*\* `--cert` is only required if you are not using the `-a` preset.*

### Example: Custom Shell and Identity

If you want to run `zsh` and use a specific identity file:

```bash
./krush --remote server.local --port 443 --cert ./ca.crt --shell /bin/zsh --ident ./my-device-id
```

### Identity Files
The `--ident <path>` option allows you to specify a device identity. Krush reads the file and uses the leading alphanumeric characters as the ID. This ID is sent to the server during the TLS handshake and used to decorate your shell prompt (e.g., `my-device-1234:\w \$ `).

## Note on Security
Krush is built for secure, authenticated communication using TLS. Always ensure that the `--cert` you provide is a trusted CA that verifies the identity of the remote server to prevent Man-in-the-Middle (MITM) attacks.

## Runtime Behavior
- **Inactivity Timeout**: To conserve resources, if no data has been received for 5 minutes, Krush will automatically tear down the connection and any running child process to prepare for a fresh reconnection attempt.
- **Shutdown**: Sending `SIGTERM` or `int` (Ctrl+C) will trigger a clean shutdown, closing the TLS session and terminating any child processes.

---
*Happy Krushing!*

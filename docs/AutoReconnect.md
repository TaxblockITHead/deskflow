# Reconnecting automatically when the network changes

When a computer joins another Wi-Fi network, or the router hands out new addresses, the server and the clients get new IP addresses. A client configured with the server's old IP address can then no longer reach it.

This fork lets the client find its server again by itself. You don't need to type in the new IP address or reconnect by hand.

## What you need

- **The same build on all computers** (this fork's installers).
- **TLS encryption turned on** on the server and the clients. It is on by default (Settings → *Enable TLS encryption*).
- **One normal connection first.** The client learns which server is its own by connecting to it once with the new build.
- **Both computers on the same local network** after the change, for example the same Wi-Fi or office network. A client can't find a server on a different network, behind another router, or over the internet.

Nothing needs to be set up. Keep the server's IP or name in the client as before; it is still tried first.

## What happens

1. **The client remembers its server.** After every successful connection, the client stores the server's TLS certificate fingerprint in `tls/last-server`. This is the same fingerprint you confirmed when you first connected. It identifies the server and doesn't change when its IP address changes.
2. **The connection breaks.** The network changes, and the client can't reach the server at the old address.
3. **The client asks the network.** After each failed attempt (about every 3 seconds), the client sends a short message to every network it is connected to: *"Is the server with fingerprint X here?"* The message is a UDP broadcast on port 24800, the Deskflow port.
4. **The server answers.** A running server listens on UDP port 24800. It only answers when asked for **its own** fingerprint: *"Yes, I'm here, connect on port 24800."*
5. **The client connects to the new address.** It connects to the address the answer came from. TLS checks the server's certificate as always, so it only connects if it really is the same server.

The client log shows:

```
found server "DESKTOP-0AQN90E" at its new address 192.168.1.42, connecting
```

The client also keeps trying the configured address. If you go back to the old network, it reconnects there as well.

## Why the certificate fingerprint and not the MAC address

| | MAC address | TLS certificate fingerprint |
|---|---|---|
| Stays the same on another Wi-Fi | **No.** macOS, iOS, Android and Windows use a private, random Wi-Fi MAC address per network, and some change it regularly. | Yes. It belongs to the Deskflow installation. |
| Can be faked | Yes, any device can claim any MAC address. | No. Only the real server has the matching private key, and TLS checks it. |
| Needs scanning the network | Yes. Every address on the network has to be probed to find a MAC address. | No. One broadcast message; only the right server answers. |
| Who starts the connection | The server would have to find the client, but in Deskflow clients connect to the server. | The client finds the server and connects, as always. |
| Already set up | No. | Yes. Every computer already has one, and clients already trust the server's. |

## Security

- The server only answers requests that name its own fingerprint, so it doesn't reveal itself to devices that don't already know it.
- The client only accepts answers carrying the fingerprint it expects. Answers from other Deskflow servers are ignored.
- A forged answer can't make the client connect to another server: the TLS check fails and the connection is refused. At worst, a device on the same network could delay the reconnection.
- Without TLS encryption the feature is off, because there would be nothing to prove the server's identity.

## Troubleshooting

| Problem | What to check |
|---|---|
| The client doesn't find the server | Both computers on the same Wi-Fi or network? Some guest and public Wi-Fi networks block devices from talking to each other ("client isolation"); then Deskflow can't work there at all. |
| Log says `not looking for the server on the network, this needs tls encryption to be enabled` | Turn TLS on, on the server and the client. |
| Log says `not looking for the server on the network, no trusted server yet` | Connect once by IP address or name, and accept the server's fingerprint. |
| Server log says `unable to listen for clients looking for this server on udp port 24800` | Another program is using UDP port 24800. |
| Server log says `clients can't find this server when its address changes, unable to read its tls certificate` | Regenerate the certificate in Settings; clients must then accept the new fingerprint once. |
| Windows firewall blocks it | The Deskflow installer allows `deskflow-core.exe` through the firewall. If Deskflow was not installed with the installer, allow UDP and TCP port 24800 for it. |
| macOS asks to find devices on the local network | Click **Allow**. If you clicked Don't Allow: System Settings → Privacy & Security → Local Network → turn on Deskflow. |

Set **Settings → Log level → Debug** to see each step, for example `looking for the server on 3 network(s)` on the client and `client 192.168.1.20 is looking for this server, answering` on the server.

## For developers

| File | What it does |
|---|---|
| `src/lib/deskflow/ServerDiscovery.h/.cpp` | Discovery messages (JSON in one UDP datagram), the server's `Responder` and the client's `Finder`. Both live in the core's main thread, which runs the Qt event loop; the Deskflow event loop in the core thread calls them through thread-safe methods. |
| `src/lib/deskflow/ClientApp.cpp` | `searchForServer()` after a failed connection; `useDiscoveredServer()` before the next attempt adds the found address to the list of server addresses and tries it. |
| `src/lib/deskflow/ServerApp.cpp` | Starts the responder when the server starts listening, stops it with the server. |
| `src/lib/net/SecureSocket.cpp` | Remembers the server's fingerprint after the TLS check succeeds. |
| `src/lib/net/SecureUtils.h/.cpp` | `certFileFingerprint()` reads the server's own fingerprint from its certificate file; `rememberServerFingerprint()` writes `tls/last-server`. |
| `src/lib/common/Settings.h/.cpp` | `Settings::tlsLastServerDb()`, the path of `tls/last-server`. |
| `src/unittests/deskflow/ServerDiscoveryTests.h/.cpp` | Message format, rejecting invalid and oversized data, a real request and answer over the loopback network, ignoring other servers and forged answers. |

Messages:

```
request (client → broadcast, UDP port 24800):
  {"deskflow": "discover", "version": 1, "fingerprints": ["v2:sha256:<hex>", ...]}

answer (server → client):
  {"deskflow": "server", "version": 1, "fingerprint": "v2:sha256:<hex>", "port": 24800, "name": "<computer name>"}
```

The client asks for the server it last connected to (`tls/last-server`). If there is none yet, it asks for all servers in `tls/trusted-servers` (at most 32).

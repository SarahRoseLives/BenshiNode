# BenshiNode Go server

This is a standalone Go implementation of the BenshiNode link
server/reflector. It is wire-compatible with the C++ `BenshiNode.exe --server`
and the C++ link client, so existing radio nodes can connect to either server.

The Go server handles the reflector: TCP authentication, UDP session
tracking, heartbeat replies, talker arbitration, and relaying opaque G.711
u-law audio frames. It can also serve an optional web dashboard with live
stats and a browser audio listener. It does not talk to a radio or implement
the BenshiNode client.

## Build

Go 1.22 or newer is required.

```powershell
cd go
go build -o ..\build\Release\benshinode-server.exe ./cmd/benshinode-server
go test ./...
cd ..
```

## Run

The server uses its own configuration file, `server.ini`, by default. It does
not read the node application's `BenshiNode.ini` unless you explicitly pass
`--config`. Create the local file from the tracked example and edit the shared
key:

```powershell
Copy-Item config\server.ini server.ini
notepad server.ini
```

Then run from the repository root:

```powershell
.\build\Release\benshinode-server.exe
```

Or run the Go command directly from the repository root:

```powershell
go run .\go\cmd\benshinode-server
```

Options:

- `--config <file>` - server configuration path (default `server.ini`)
- `--verbose` - force debug logging
- `--web <addr>` - enable the web dashboard and live audio on `host:port` or a
  bare port; an empty value or `--web true` uses `[WEB]`, and `--web off`
  disables a config-enabled dashboard

Examples:

```powershell
# Local-only dashboard on http://127.0.0.1:8080
.\build\Release\benshinode-server.exe --web 127.0.0.1:8080

# Dashboard on all interfaces, port 8080
.\build\Release\benshinode-server.exe --web :8080

# Enable from the configuration file instead
# [WEB]
# ENABLED=1
# BIND=127.0.0.1
# PORT=8080
```

The process runs until Ctrl+C. It listens on TCP `SERVER/PORT` and UDP
`SERVER/AUDIO_PORT` (defaults `7300` and `7301`). Only those two ports need to
be reachable from clients. The optional dashboard uses its own TCP port.

`server.ini` is ignored by git because it contains the shared key.
`config/server.ini` is the tracked example.

## Configuration

The Go server uses these keys:

```ini
[SERVER]
PORT=7300
AUDIO_PORT=7301
KEY=use-a-long-random-shared-key
MAX_CLIENTS=16

[WEB]
ENABLED=0
BIND=127.0.0.1
PORT=8080

[LOG]
LEVEL=info
FILE=BenshiNode-reflector.log
```

`KEY` is a shared authentication key, not audio encryption. Audio is sent in
the clear, as is normal for amateur radio. An empty key disables HMAC checking
and allows anyone to connect; the server logs a warning in that case.

`MAX_CLIENTS` counts authenticated sessions plus in-flight authentication
handshakes. The C++ server only counts completed sessions.

## Web dashboard and live audio

When enabled, the Go server exposes:

- `/` - dashboard with uptime, session list, active talker, packet counters,
  listener count, and a recent activity feed
- `/api/stats` - the same data as JSON
- `/audio` - a live WAV stream of the active talker (for external players)
- `/audio/raw` - raw G.711 u-law frames as an HTTP stream
- `/audio/ws` - WebSocket that sends one u-law frame per message (dashboard)
- `/audio-worklet.js` - browser AudioWorklet that decodes and buffers live audio
- `/healthz` - plain `ok` health check

The dashboard polls `/api/stats` once per second. Its **Listen live** button
opens `/audio/ws`, receives one 20 ms u-law frame per WebSocket message, and
forwards the frames to an AudioWorklet. The worklet decodes, resamples, and
buffers the audio on the audio thread, so main-thread jank cannot create gaps.
It targets a ~80 ms jitter buffer and adjusts the playback rate by at most ±2%
to compensate for clock drift. Frames are dropped if the buffer grows beyond
roughly 300 ms.

The WAV endpoint paces frames at 20 ms and sends silence while nobody is
talking, so external players receive continuous real-time audio. The raw HTTP
endpoint forwards frames as they arrive. Multiple listeners can connect at
once, and every listener hears the active talker being relayed.

The `[WEB]` section and the `--web` flag are handled only by the Go server; the
C++ server ignores them.

> **Security:** the dashboard and audio stream have no authentication and do
> not use TLS. `BIND=127.0.0.1` keeps them local. To expose them remotely, put
> the web server behind a reverse proxy with authentication/TLS, or restrict
> access with a firewall.

## Protocol summary

The implementation matches the C++ code in `src/link`.

### TCP authentication

1. Server sends a 40-byte challenge:
   - `0..3`: magic `"BNS1"` (`0x31534E42`, little-endian)
   - `4`: protocol version `1`
   - `8..39`: 32-byte random nonce
2. Client sends a 52-byte response:
   - `0..3`: magic `"BNS1"`
   - `4..18`: callsign, up to 15 bytes, NUL padded
   - `20..51`: `HMAC-SHA256(KEY, nonce)`
3. Server replies with 12 bytes:
   - `0..3`: magic `"BNOK"` (`0x4B4F4E42`)
   - `4..7`: assigned session ID (little-endian)
   - `8..9`: UDP audio port (little-endian)

Failures use an 8-byte `"BNER"` (`0x52454E42`) reply. The server allows four
seconds for the handshake and then closes the TCP connection.

### UDP audio

Every datagram starts with a 16-byte header:

```text
0..3   magic "BND1" (0x31444E42, little-endian)
4      packet type: 1=Register, 2=Audio, 3=End, 4=Heartbeat, 5=ServerEnd
5      flags: 0x01=Start, 0x02=End
6..7   payload length (little-endian)
8..11  session ID (little-endian)
12..15 sequence number (little-endian)
```

Audio payloads are 20 ms G.711 u-law frames (160 bytes at 8 kHz). The server
relays them unchanged, using the sender's session ID, to every other session
that has sent at least one UDP packet. Payloads are capped at 1024 bytes.

Session behavior:

- A session is created by TCP authentication and expires after 30 seconds
  without UDP traffic.
- `Register` and any other known packet binds the session to the sender's UDP
  address.
- `Heartbeat` gets an empty heartbeat reply and keeps the session alive. Clients
  send one every five seconds.
- Only one talker is relayed at a time. A new talker takes over after 600 ms
  without audio from the previous talker.
- `End` clears the active talker and is relayed to the other sessions.

## Source layout

```text
go/
  cmd/benshinode-server/   command-line entry point and INI wiring
  internal/config/         BenshiNode INI reader
  internal/link/           protocol, HMAC handshake, reflector, stats, audio
  internal/logging/        leveled logger
  internal/web/            dashboard, stats, WebSocket audio, AudioWorklet
```

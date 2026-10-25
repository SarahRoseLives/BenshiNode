# BenshiNode

BenshiNode is a Windows C++ simplex radio node for the Vero VR-N76 handheld,
the VR-N7600 mobile, and compatible Benshi Bluetooth radios. It turns the radio
into a small SVXLink-style voice node with DTMF control, parrot mode, CW or
spoken identification, BenshiNode linking, outbound EchoLink, and a
bidirectional Discord voice bridge.

Radio callsign used by the current configuration: **N0CALL**.

## Current status

Working:

- VR-N76 / VR-N7600 Bluetooth Classic connection (control + SBC audio)
- Radio RX/TX audio, squelch/VOX, hang time, TX timeout and forced unkey
- DTMF decoding with DTMF audio muted from parrot/link recordings
- Parrot/repeat with `1#`
- CW or WAV station identification
- Optional courtesy tone
- Single/dual-watch configuration and memory-channel selection
- APRS location-sharing disable
- BenshiNode link server/reflector (`--server`)
- BenshiNode link client (`--connect`)
- Outbound EchoLink with directory lookup, GSM 06.10 audio, and DTMF dialing
- Optional Discord voice-channel bridge with DTMF join/leave control
- Optional Piper female voice for help, callsign spelling, and command prompts
- Native Windows GUI with status, DTMF keypad, EchoLink/Discord controls, and live log
- Windows build and automated protocol/codec tests

Not yet implemented:

- Incoming EchoLink connections
- EchoLink proxy support
- AllStarLink / IAX2
- SVXLink reflector interoperability
- Dynamic DTMF commands for the BenshiNode link server
- Full SVXLink module/event system

EchoLink and Discord are disabled by default. No EchoLink station is contacted
unless `ECHOLINK/ENABLED=1`, `--echolink` is used, or a DTMF dial command is
sent. Discord is inactive until a token, guild, and voice channel are
configured.

## Requirements

- Windows 10 or 11, x64
- Visual Studio 2022 C++ build tools
- CMake 3.20 or newer
- A paired VR-N76, VR-N7600, or compatible Benshi radio
- Optional: Piper and a female voice model for spoken prompts

The radio exposes two Bluetooth Classic RFCOMM services:

- Control: SPP `00001101-0000-1000-8000-00805F9B34FB`
- Audio: vendor "BS AOC" `39144315-32FA-40DB-85ED-FBFEBA2D86E6`
  (falls back to generic audio `00001203-0000-1000-8000-00805F9B34FB`)

## Pairing a radio

The easiest path is the native GUI:

1. Put the radio in Bluetooth pairing mode.
2. Open `BenshiNodeGUI.exe` and click **Pair radio**.
3. Click **Refresh**, select each radio entry, and click **Pair selected**.
   Windows may list separate control and audio entries; pair both.
4. Select the entry BenshiNode should use and click **Use this radio**.
5. Click **Restart** in the main window.

The radio normally appears as **two** Bluetooth devices: an audio device and a
control device. If only the audio device is paired, BenshiNode will report that
the RFCOMM service could not be found. Pairing from Windows Settings still
works as a fallback.

## Build

From a Visual Studio Developer PowerShell:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The executables are `build\Release\BenshiNode.exe` (console) and
`build\Release\BenshiNodeGUI.exe` (native GUI launcher).

## Quick start

After building:

```powershell
# Create BenshiNode.ini and optionally install the female Piper voice
.\tools\setup.cmd -WithPiper

# Edit RADIO/MAC, NODE/CALLSIGN, and any optional service credentials
notepad BenshiNode.ini

# Start the native GUI
.\build\Release\BenshiNodeGUI.exe
```

The `.cmd` wrappers use `-ExecutionPolicy Bypass` so they also work when
PowerShell script execution is disabled by local policy.

`BenshiNodeGUI.exe` starts `BenshiNode.exe --gui` without a console window.
You can also run `.\build\Release\BenshiNode.exe --gui` directly.

## Find the radio

```powershell
.\build\Release\BenshiNode.exe --list
```

Radio candidates are marked. Put the control device's address in the
configuration file.

## Configuration

Copy `config\example.ini` to `BenshiNode.ini` and edit it. The local
`BenshiNode.ini` is intentionally ignored by git because it contains Bluetooth
and EchoLink credentials.

Important settings:

```ini
[RADIO]
MAC=AA:BB:CC:DD:EE:FF
DUAL_WATCH=0          ; 0=single, A/1=VFO A, B/2=VFO B
# CHANNEL=30          ; optional 0-based memory channel; radio display is +1
APRS=0                ; disable APRS location sharing
SBC_BITPOOL=18        ; radio-compatible SBC quality
TX_LEAD_MS=100        ; keep the radio playout buffer shallow
STATUS_POLL_MS=500
CONNECT_RETRY_MS=5000

[NODE]
CALLSIGN=N0CALL
DTMF_PREFIX=          ; empty = commands like 0#, 1#, 9#
DTMF_SUFFIX=#
PARROT_MODE=dtmf      ; off | dtmf | auto
SQUELCH_MODE=auto     ; radio | vox | auto
VOX_THRESHOLD_DB=-38
ID_INTERVAL_SEC=600
ID_MIN_GAP_SEC=120
CW_WPM=20
CW_TONE_HZ=700
WAV_DIR=clips
```

On connect BenshiNode:

1. Applies `DUAL_WATCH` and `APRS`.
2. Selects `CHANNEL` only when it is configured.
3. Starts the node.

BenshiNode **never rewrites channel memory**. If `CHANNEL` is set, that existing
memory channel is selected as-is. If it is unset or blank, BenshiNode does not
change the radio channel. Use the GUI Channel box to select a memory channel or
clear the selection, or use `--dump` to inspect the radio.

`CHANNEL` is 0-based. The radio display is normally one higher. Leaving
`CHANNEL` unset keeps the radio on its current channel. Setting `FREQUENCY`
uses the VFO fallback instead.

## Running

```powershell
# Radio and status information
.\build\Release\BenshiNode.exe --probe

# Inspect memory channels and APRS state
.\build\Release\BenshiNode.exe --dump

# Receive audio and optionally record a 32 kHz mono WAV
.\build\Release\BenshiNode.exe --monitor rx.wav

# Transmit a 1 kHz test tone (only with an appropriate RF setup)
.\build\Release\BenshiNode.exe --test-tx

# Send the configured callsign in CW
.\build\Release\BenshiNode.exe --tap-id

# Force the radio out of transmit
.\build\Release\BenshiNode.exe --unkey

# Run the simplex node in the console
.\build\Release\BenshiNode.exe

# Run the native GUI
.\build\Release\BenshiNode.exe --gui
```

Use `--config <file>` for a different configuration and `--verbose` for debug
logging. Press Ctrl+C to stop.

## Native GUI

`BenshiNodeGUI.exe` is the no-console launcher for the native Windows window.
It starts the same node engine and configuration as the console version.

The window shows radio connection, channel, RSSI, TX/RX state, Piper status,
EchoLink and Discord state, and a live log. It provides quick command buttons
for help (`0#`), identify (`3#`), parrot (`1#`), reset (`9#`), force unkey, and
node stop, plus an on-screen DTMF keypad and EchoLink dial field. All controls
use the same no-star command codes as the radio keypad.

The **Pair radio** button scans Bluetooth, pairs selected devices, and saves the
selected radio address. Windows may show separate control and audio entries for
the radio; pair both, then select the entry BenshiNode should use. No `.ini`
editing is required.

The **Settings** window lists every section and key in the configuration as
individual fields. Select a field, edit its value, and use Save; optional path
fields have a Browse button. This covers radio, channel, DTMF, Piper, EchoLink,
Discord, link, logging, and advanced options. Saving creates a
`BenshiNode.ini.bak` backup first.

The main window also has a **Channel** box. Enter a memory channel ID and click
Apply to select it live and save it for future starts. Leave it blank and click
Apply to stop forcing a channel; the radio then stays on its own current
channel. Use the **Restart** button after changing settings that require a
reload.

## DTMF commands

Default command form is `<n>#` (no leading star):

| Command | Action |
| --- | --- |
| `1#` | Toggle parrot mode |
| `0#` | Play spoken help, `clips\help.wav`, or CW help |
| `2#` | Connect the configured BenshiNode link |
| `3#` | Send ident now |
| `4#` | Connect to the configured EchoLink node |
| `4<node>#` | Dial any EchoLink node, e.g. `4179729#` |
| `5#` | Disconnect EchoLink |
| `6#` | Join the configured Discord voice channel |
| `7#` | Leave the Discord voice channel |
| `8#` | Disconnect all networks (link, EchoLink, Discord) |
| `9#` | Reset/cancel pending transmissions |

The EchoLink dial prefix is configurable with `[ECHOLINK] DTMF_PREFIX` and
defaults to `4`. DTMF tones are removed from parrot and network audio.

## Station identification

The ident clip is selected in this order:

1. `clips\ident.wav`
2. `clips\<CALLSIGN>.wav` (for example `clips\N0CALL.wav`)
3. Piper-spelled callsign, if `[PIPER] ENABLED=1` and `SPELL_CALLSIGN=1`
4. Generated CW from `CALLSIGN`

Piper spells the callsign as individual character clips, then inserts a short
gap and plays them in sequence, SVXLink-style. WAV files may be 8-bit or 16-bit
PCM, mono or stereo, and are resampled to 32 kHz. The radio still needs its own
RF identification as required by local regulations.

## BenshiNode linking

BenshiNode can host a reflector-style server, allowing radio nodes to link
without opening inbound ports on the clients.

### Host

```ini
[SERVER]
PORT=7300
AUDIO_PORT=7301
KEY=use-a-long-random-shared-key
MAX_CLIENTS=16
```

```powershell
.\build\Release\BenshiNode.exe --config config\server.ini --server
```

Only the server needs inbound **TCP 7300** and **UDP 7301**. Clients make
outbound connections.

A standalone Go implementation of the server/reflector is available in `go/`.
It is wire-compatible with the C++ client and uses its own `server.ini`
(copied from `config/server.ini`). It adds an optional web dashboard with live
stats and browser audio listening (`--web :8080`). See `go/README.md` for
build and run instructions.

### Client

In the native GUI, use the **BenshiNode link** panel: enter the server as
`host:port`, enter the shared key, and click **Connect**. Click **Disconnect**
to leave. The server and key are saved for convenience. When `[LINK] ENABLED=1`
and `SERVER`/`KEY` are configured, the link also connects automatically at
startup. A BenshiNode link has priority over Discord: connecting it
automatically leaves the Discord voice channel and disables Discord auto-join
while the link is active. EchoLink must still be disconnected first. DTMF `2#`
connects the configured link and `8#` disconnects all networks.

The same connection can be configured for console/startup use:

```ini
[LINK]
ENABLED=1
SERVER=your.server.example:7300
KEY=use-a-long-random-shared-key
```

or:

```powershell
.\build\Release\BenshiNode.exe --connect your.server.example:7300
```

The client reconnects, relays local RF audio, plays incoming server audio on the
radio, gives local RF priority, and suppresses DTMF tones from the link.

Link audio uses G.711 u-law, 8 kHz mono, 20 ms UDP frames (about 64 kbit/s each
way). The TCP handshake uses HMAC-SHA256 for access control. Audio is sent in
the clear, as appropriate for amateur radio.

## EchoLink

EchoLink uses the directory at `servers.echolink.org:5200`, the legacy
RTP/RTCP QSO protocol, and GSM 06.10 audio.

```ini
[ECHOLINK]
ENABLED=0
DTMF=1
DTMF_PREFIX=4
CALLSIGN=AD8NT
PASSWORD=your-echolink-password
NAME=AD8NT
INFO=BenshiNode
NODE=179729
DIRECTORY=servers.echolink.org:5200
PORT_BASE=5198
```

`ENABLED=0` means no EchoLink connection at startup. With `DTMF=1`, the node
starts idle and waits for keypad commands:

- `4#` connects to `NODE`
- `4<node>#` dials any node
- `5#` disconnects

`--echolink <node>` explicitly connects at startup.

EchoLink binds local UDP `5198` and `5199`. NAT port forwarding may be needed if
the remote station cannot return audio. EchoLink proxy support is not
implemented yet.

EchoLink audio is sent in the clear. There is no encryption, and none is needed
for amateur radio. Stations remain responsible for identification and for
following EchoLink and national regulations.

## Piper voice prompts

BenshiNode can use Piper to speak help, station identification, startup,
network link state, and command acknowledgements. The default voice is the
clear female `en_US-lessac-medium` model.

No Python installation is required. The Windows Piper download is a native C++
executable using ONNX Runtime and espeak-ng. Install the runtime and voice with:

```powershell
.\tools\install-piper.cmd
```

or install the config, Piper runtime, and voice together:

```powershell
.\tools\setup.cmd -WithPiper
```

The `.ps1` scripts can also be run directly with
`powershell -ExecutionPolicy Bypass -File ...` if your policy blocks scripts.

Piper is downloaded to `tools\piper` and is not committed to git. Then add:

```ini
[PIPER]
ENABLED=1
EXE=tools\piper\piper.exe
MODEL=tools\piper\voices\en_US-lessac-medium.onnx
CACHE_DIR=tts-cache
LENGTH_SCALE=1.0
SENTENCE_SILENCE=0.05
SPELL_CALLSIGN=1
STARTUP_PROMPT=1
TIMEOUT_MS=30000
# HELP_TEXT=Custom help text.
# STARTUP_TEXT=BenshiNode ready.
# LINK_CONNECTED_TEXT=Connected to Network.
# LINK_DISCONNECTED_TEXT=Disconnected from Network.
```

The node speaks **Connected to Network** when a BenshiNode link is established
and **Disconnected from Network** when it drops. If Piper is unavailable, it
falls back to the normal beep acknowledgement.

Generated WAV clips are cached in `tts-cache`. The first startup takes a few
seconds while the cache is built; later startups reuse the cached clips. The
cache is keyed by the voice model, pacing settings, and text, so changing a
prompt generates only the new clip.

Piper is optional. If it is disabled or unavailable, BenshiNode falls back to
`clips\help.wav`/`clips\ident.wav`, CW, or the normal beep acknowledgements.
The spoken callsign is assembled from cached letter and digit clips.

Other Piper voices can be used by pointing `MODEL` at another `.onnx` model and
placing its `.onnx.json` beside it. Piper and its voice models are downloaded
separately and remain subject to their own licenses.

## Discord

BenshiNode bridges radio audio to one Discord voice channel in both directions.
Radio RX audio is sent to Discord; Discord audio is mixed to mono and played on
the radio. A BenshiNode link has priority over Discord: starting a link leaves
the Discord voice channel and blocks Discord join until the link disconnects.
EchoLink may stay configured for DTMF use, but BenshiNode refuses to join
Discord while EchoLink is connected, and refuses to start EchoLink while Discord
is connected.

Create a Discord application and bot, invite the bot with **Connect** and
**Speak** permissions for the target voice channel, then copy the bot token,
guild ID, and channel ID into the ignored `BenshiNode.ini`. No privileged
message-content intent is required for the voice bridge.

Discord now negotiates DAVE end-to-end encryption on voice channels that require
it. D++ logs a six-part `E2EE Privacy Code` when the encrypted voice session is
ready. D++ may also log `frame is not encrypted and pass through is disabled` for
legacy/unencrypted voice frames; this does not stop normal radio or Discord
audio.

```ini
[DISCORD]
ENABLED=0             ; 1 = join the voice channel at startup
DTMF=1                ; allow 6# / 7# control
TOKEN=your-bot-token
GUILD_ID=123456789012345678
CHANNEL_ID=123456789012345678
```

With `ENABLED=0` and `DTMF=1`, the bot connects to Discord but stays out of the
voice channel until `6#` is sent. Set `ENABLED=1` to auto-join at startup.
`7#` leaves the channel. Radio RX audio is resampled from 32 kHz to Discord's
48 kHz stereo, and Discord audio is mixed to mono and resampled back to 32 kHz
for the radio.

The bot token is a credential. Keep it in the ignored `BenshiNode.ini`, never in
a tracked file. D++ handles Discord voice encryption and Opus; audio sent to
Discord is protected by Discord's voice transport and DAVE E2EE when active.
The D++ runtime DLLs and `ca-bundle.crt` are copied next to `BenshiNode.exe` by
the build.

## Radio protocol notes

- Control: GAIA framing `FF 01 00 LEN [group16][cmd16][data...]`.
- Replies set bit 15 in the command.
- Audio: `0x7E`-delimited SBC frames on the BS AOC RFCOMM channel.
- TX audio command `0x00`; RX commands `0x00` / `0x03`; TX echo `0x09`;
  end-of-audio `0x01`.
- SBC: 32 kHz, 16-bit mono, 16 blocks, 8 subbands.
- Sending TX audio keys the radio. The end-of-audio frame unkeys it.
- BenshiNode re-sends the end frame and has an explicit unkey watchdog.

## Project layout

```text
src/
  bluetooth/   WinRT RFCOMM transport
  radio/       GAIA control, status, SBC audio
  node/        squelch, DTMF, parrot, tones, ID, TX engine
  link/        BenshiNode server/client, G.711, resampling, auth
  echolink/    EchoLink directory, SDES/RTP, GSM QSO
  discord/     D++ bot and Discord voice bridge
  tts/         Piper prompt generation and callsign spelling
  util/        WAV output
third_party/
  sbc/         Google libsbc (Apache-2.0)
  gsm/         TU Berlin GSM 06.10 codec
  dpp/         D++ Discord library, prebuilt Windows x64 (Apache-2.0)
tests/         protocol, codec, DTMF and audio tests
config/        example node and server configurations
```

## Third-party code

- `third_party/sbc`: Google libsbc, Apache-2.0, see
  `third_party/sbc/LICENSE`.
- `third_party/gsm`: TU Berlin GSM 06.10 reference codec, see
  `third_party/gsm/COPYRIGHT`.
- `third_party/dpp`: D++ Discord library, prebuilt Windows x64 release,
  Apache-2.0, see `third_party/dpp/LICENSE`.
- `third_party/dpp/ca-bundle.crt`: Mozilla CA root bundle from
  `https://curl.se/ca/cacert.pem`, MPL-2.0. BenshiNode points OpenSSL at this
  bundle so D++ can verify Discord's HTTPS certificates on Windows.
- Piper and the `en_US-lessac-medium` voice are optional, downloaded by
  `tools/install-piper.ps1`, and not part of this repository. See the Piper and
  voice-model projects for their licenses.

The `references/` directory used during development is not part of this
repository.

## Troubleshooting

- **No RFCOMM service**: pair the control device as well as the audio device,
  and make sure the radio is powered on.
- **Audio channel fails**: power the radio on before starting BenshiNode.
- **No RX audio**: check `--monitor`, radio volume, and Bluetooth pairing.
- **Node steps on local RF**: adjust `SQUELCH_MODE`, `VOX_THRESHOLD_DB`, and
  `VOX_HANG_MS`.
- **Radio still shows APRS or dual watch**: set `DUAL_WATCH=0` and `APRS=0`, then
  run `--dump`.
- **EchoLink will not connect**: verify the EchoLink callsign/password, the
  target node ID, and UDP 5198/5199 reachability.
- **Radio stuck transmitting**: run `--unkey`; BenshiNode also has automatic
  end-frame and status-based unkey recovery.
- **Discord bot will not join**: verify the bot token, guild/channel IDs, and
  that the bot has Connect/Speak permission. Voice requires UDP access to
  Discord; restrictive firewalls can prevent audio. If HTTPS fails with
  `Malformed HTTP response`, make sure `ca-bundle.crt` is next to
  `BenshiNode.exe` or set `SSL_CERT_FILE` to a valid CA bundle.
- **Piper prompts are silent or CW**: check `PIPER/ENABLED`, `EXE`, and `MODEL`
  paths. Run `piper.exe --help` and test a sentence through Piper manually.
  Delete `tts-cache` to force regeneration after changing voice settings.

## Safety

EchoLink and BenshiNode linking can transmit over the air. Confirm the channel,
power, and legal operating status before enabling a link. The example
configuration keeps EchoLink disabled and does not connect to any station.

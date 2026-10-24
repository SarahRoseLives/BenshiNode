# BenshiNode

BenshiNode is a Windows C++ simplex radio node for the Vero VR-N76 and
compatible Benshi Bluetooth handhelds. It turns the radio into a small
SVXLink-style voice node with DTMF control, parrot mode, CW identification,
BenshiNode linking, and outbound EchoLink.

Radio callsign used by the current configuration: **K8SDR**.

## Current status

Working:

- VR-N76 Bluetooth Classic connection (control + SBC audio)
- Radio RX/TX audio, squelch/VOX, hang time, TX timeout and forced unkey
- DTMF decoding with DTMF audio muted from parrot/link recordings
- Parrot/repeat with `*1#`
- CW or WAV station identification
- Optional courtesy tone
- Single/dual-watch configuration and memory-channel selection
- APRS location-sharing disable
- BenshiNode link server/reflector (`--server`)
- BenshiNode link client (`--connect`)
- Outbound EchoLink with directory lookup, GSM 06.10 audio, and DTMF dialing
- Windows build and automated protocol/codec tests

Not yet implemented:

- Incoming EchoLink connections
- EchoLink proxy support
- AllStarLink / IAX2
- SVXLink reflector interoperability
- Dynamic DTMF commands for the BenshiNode link server
- Full SVXLink module/event system

EchoLink is disabled by default. No EchoLink station is contacted unless
`ECHOLINK/ENABLED=1`, `--echolink` is used, or a DTMF dial command is sent.

## Requirements

- Windows 10 or 11, x64
- Visual Studio 2022 C++ build tools
- CMake 3.20 or newer
- A paired VR-N76 / compatible Benshi radio

The radio exposes two Bluetooth Classic RFCOMM services:

- Control: SPP `00001101-0000-1000-8000-00805F9B34FB`
- Audio: vendor "BS AOC" `39144315-32FA-40DB-85ED-FBFEBA2D86E6`
  (falls back to generic audio `00001203-0000-1000-8000-00805F9B34FB`)

## Pairing on Windows

The radio normally appears as **two** Bluetooth devices: an audio device and a
control device. Pair both.

1. Remove any old pairing for the radio in Windows Settings.
2. Put the radio into pairing mode and pair the **audio** device.
3. Re-enable pairing on the radio.
4. Add a device again, choose "Show all devices", and pair the **control**
   device with the same radio name.

If only the audio device is paired, BenshiNode will report that the RFCOMM
service could not be found.

## Build

From a Visual Studio Developer PowerShell:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The executable is `build\Release\BenshiNode.exe`.

## Find the radio

```powershell
.\build\Release\BenshiNode.exe --list
```

VR-N76 candidates are marked. Put the control device's address in the
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
CHANNEL=30            ; 0-based memory channel; radio display is +1
APRS=0                ; disable APRS location sharing
SBC_BITPOOL=18        ; radio-compatible SBC quality
TX_LEAD_MS=100        ; keep the radio playout buffer shallow
STATUS_POLL_MS=500
CONNECT_RETRY_MS=5000

[NODE]
CALLSIGN=K8SDR
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
2. Selects the existing `CHANNEL` memory channel.
3. Starts the node.

BenshiNode **never rewrites channel memory**. Channel 30 is selected as-is;
it is the radio's display channel 31 in the current setup. Use `--dump` to see
the selected channel, frequency, name, and APRS state.

`CHANNEL` is 0-based. The radio display is normally one higher. Leaving
`CHANNEL` unset enables the VFO fallback using `FREQUENCY`.

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

# Run the simplex node
.\build\Release\BenshiNode.exe
```

Use `--config <file>` for a different configuration and `--verbose` for debug
logging. Press Ctrl+C to stop.

## DTMF commands

Default command form is `*<n>#`:

| Command | Action |
| --- | --- |
| `*1#` | Toggle parrot mode |
| `*2#` | Play help (CW or `clips\help.wav`) |
| `*3#` | Send ident now |
| `*0#` | Reset/cancel pending transmissions |
| `*4#` | Connect to the configured EchoLink node |
| `*4<node>#` | Dial any EchoLink node, e.g. `*4179729#` |
| `*5#` | Disconnect EchoLink |

The EchoLink dial prefix is configurable with `[ECHOLINK] DTMF_PREFIX` and
defaults to `*4`. DTMF tones are removed from parrot and network audio.

## Station identification

The ident clip is selected in this order:

1. `clips\ident.wav`
2. `clips\<CALLSIGN>.wav` (for example `clips\K8SDR.wav`)
3. Generated CW from `CALLSIGN`

WAV files may be 8-bit or 16-bit PCM, mono or stereo, and are resampled to
32 kHz. The radio still needs its own RF identification as required by local
regulations.

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

### Client

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
DTMF_PREFIX=*4
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

- `*4#` connects to `NODE`
- `*4<node>#` dials any node
- `*5#` disconnects

`--echolink <node>` explicitly connects at startup.

EchoLink binds local UDP `5198` and `5199`. NAT port forwarding may be needed if
the remote station cannot return audio. EchoLink proxy support is not
implemented yet.

EchoLink audio is sent in the clear. There is no encryption, and none is needed
for amateur radio. Stations remain responsible for identification and for
following EchoLink and national regulations.

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
  util/        WAV output
third_party/
  sbc/         Google libsbc (Apache-2.0)
  gsm/         TU Berlin GSM 06.10 codec
tests/         protocol, codec, DTMF and audio tests
config/        example node, server, and client configurations
```

## Third-party code

- `third_party/sbc`: Google libsbc, Apache-2.0, see
  `third_party/sbc/LICENSE`.
- `third_party/gsm`: TU Berlin GSM 06.10 reference codec, see
  `third_party/gsm/COPYRIGHT`.

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

## Safety

EchoLink and BenshiNode linking can transmit over the air. Confirm the channel,
power, and legal operating status before enabling a link. The example
configuration keeps EchoLink disabled and does not connect to any station.

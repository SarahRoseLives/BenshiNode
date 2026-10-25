# BenshiNode Android

Flutter client that turns an Android phone into a BenshiNode using a
Bluetooth Classic VR-N76 / VR-N7600 (and compatible Benshi radios).

## Current features

- Bluetooth Classic control (SPP/GAIA) and audio (BS AOC/SBC) channels
- Radio status: power, channel, RSSI, TX/RX, squelch
- DTMF command decoding and prefixless command keypad
- Parrot mode, ident, queueing, and forced unkey
- Spoken prompts synthesized with Android TTS and transmitted over RF
- BenshiNode link client (TCP authentication, UDP G.711 audio, heartbeat)
- Live log with copy support
- Settings for callsign, radio MAC, channel, squelch, link, and voice speed

## Build

```powershell
cd mobile
flutter pub get
flutter build apk --release
```

For a connected device:

```powershell
flutter run -d <device-id>
```

The Android application ID is `dev.sarahsforge.benshinode`.

## Configuration

The app writes `BenshiNode.ini` in its private documents directory on first run.
Use the in-app **Settings** screen to configure the radio MAC, callsign, link
server/key, squelch mode, and TTS speed. Pair the radio in Android Bluetooth
settings or with the **Pair radio in Bluetooth settings** button.

## Attribution

Portions of the Android Bluetooth bridge and SBC codec are adapted from
HTCommander (Apache-2.0). See [NOTICE](NOTICE) and
[third_party/HTCommander-LICENSE-APACHE-2.0.txt](third_party/HTCommander-LICENSE-APACHE-2.0.txt).

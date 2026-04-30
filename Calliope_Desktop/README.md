# Calliope Desktop

This desktop app is the Windows-side bridge for the deep-path architecture:

1. A virtual audio driver exposes a render device ("fake speaker") to Windows.
2. Calliope captures PCM from that render stream.
3. Captured audio is sent to the mobile client for playback.

## Current status

Phase 1 is in place: a WASAPI capture foundation.

- `--list`: lists active render endpoints.
- `--attach-virtual`: enables the installed virtual audio driver device node.
- `--detach-virtual`: disables the installed virtual audio driver device node.
- `--capture-default <seconds> <out_raw_pcm>`: captures loopback audio from the default render endpoint and writes raw PCM bytes.
- `--capture-auto-virtual <seconds> <out_raw_pcm>`: tries to auto-pick a virtual render endpoint by name.
- `--capture-name <name_contains> <seconds> <out_raw_pcm>`: captures from the first render endpoint whose friendly name contains the given text.

## Build

```powershell
cmake -S Calliope_Desktop -B Calliope_Desktop/build
cmake --build Calliope_Desktop/build --config Release
```

## Run examples

```powershell
.\Calliope_Desktop\build\Release\Calliope_Desktop.exe --list
.\Calliope_Desktop\build\Release\Calliope_Desktop.exe --attach-virtual
.\Calliope_Desktop\build\Release\Calliope_Desktop.exe --capture-default 10 .\capture.raw
.\Calliope_Desktop\build\Release\Calliope_Desktop.exe --capture-auto-virtual 10 .\capture_virtual.raw
.\Calliope_Desktop\build\Release\Calliope_Desktop.exe --capture-name "Virtual Audio Driver" 10 .\capture_named.raw
.\Calliope_Desktop\build\Release\Calliope_Desktop.exe --detach-virtual
```

`--attach-virtual` / `--detach-virtual` typically require running the app elevated (`Run as Administrator`).

## USB mode

The mobile app can also connect over USB by using Android's ADB reverse tunnel.

1. Connect the phone with a USB cable.
2. Make sure USB debugging is enabled.
3. Run this on the PC:

```powershell
adb reverse tcp:4010 tcp:4010
```

4. In the mobile app, switch to `USB` mode and tap `Baglan`.

The desktop app now tries to run that reverse command automatically on startup if `adb` is available in `PATH` or in a standard Android SDK location. It retries for a short while in case the phone is plugged in a little later. If it still cannot find `adb`, Wi-Fi mode keeps working unchanged and you can still run the command manually.

## Virtual audio driver source

Repository is cloned under:

- `external/Virtual-Display-Driver`

Target driver source used for this project:

- `external/Virtual-Display-Driver/Virtual-Audio-Driver (Latest Stable)`

## Deep-path roadmap

1. Add endpoint selection by device ID/name (target: virtual speaker endpoint).
2. Add real-time network streaming (framed PCM over TCP/UDP).
3. Add mobile receiver jitter buffer and live playback.
4. Add optional control channel (latency mode, reconnect, mute).

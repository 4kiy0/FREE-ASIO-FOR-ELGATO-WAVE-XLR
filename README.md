# Free ASIO for Elgato Wave XLR

A free, open-source ASIO driver for the **Elgato Wave XLR MK2** on Windows x64.
Made for low-latency recording and monitoring in REAPER (should work in any ASIO host).

> Early release. Built and compiled, but tested on very few setups so far. Feedback and logs are very welcome.

## What it is

The Wave XLR doesn't ship with an ASIO driver. This project adds one.

It's **not** a kernel driver and it does **not** replace Elgato's driver. It's a small user-mode DLL that sits on top of the normal Elgato driver and makes the Wave XLR show up as an ASIO device. Same idea as FlexASIO, but made for this one interface.

## How it works

- Opens the Wave XLR mic and headphone endpoints in **WASAPI exclusive, event-driven mode**, so audio skips the Windows mixer (no resampling, no shared-mode buffering).
- One high-priority thread (MMCSS "Pro Audio") does everything in the same pass: read mic audio → hand it to the DAW → write the DAW's output to the headphones. Input and output stay in sync because they share the same USB clock.
- The buffer size defaults to the smallest period Elgato's driver accepts in exclusive mode. That's the real floor: no ASIO driver on top of Elgato's driver can go lower.
- A small FIFO handles DAW buffer sizes that don't match the device period, plus an optional input safety margin against packet jitter.
- Formats tried in order: 24-in-32 bit, 24-bit packed, 32-bit, float, 16-bit. Audio is given to the DAW as float32.
- Reports real sample position and timestamps (ASIOTime) so latency compensation stays correct.

## Install

Step-by-step guide: [INSTALL.txt](INSTALL.txt) (also included in the release zip).

1. Download `WaveXlrAsio-x64.zip` from the [Releases](../../releases) page and unzip it (or build it yourself, see below).
2. **Close Wave Link.** Exclusive mode can't open while another app uses the device.
3. Open `mmsys.cpl` → Wave XLR, on both the **Playback** and **Recording** tabs → Properties → Advanced:
   - tick "Allow applications to take exclusive control of this device"
   - set 48000 Hz
4. Right-click `install.bat` → **Run as administrator**. It copies the DLL to `C:\Program Files\WaveXLR-ASIO\` and registers it.
5. REAPER → Preferences → Audio → Device → Audio system: **ASIO** → driver: **Wave XLR MK2 ASIO**.

## Settings

Click "ASIO Configuration" in REAPER (or edit `%APPDATA%\WaveXLR-ASIO\config.ini`):

| Key | Default | Meaning |
|---|---|---|
| `BufferFrames` | `0` | ASIO buffer in samples. `0` = smallest the Elgato driver allows |
| `SafetyPeriods` | `1` | Input margin. `0` = lowest latency, `1` = safer if you hear clicks |
| `InputMatch` / `OutputMatch` | `Wave XLR` | Part of the Windows device name to use |
| `SampleRate` | `48000` | Default sample rate |

After changing it, re-apply the device in REAPER or restart REAPER.

## Troubleshooting

Look at `%APPDATA%\WaveXLR-ASIO\log.txt`. It lists every audio device found, the device that was picked, the negotiated format, the real minimum buffer size and any error.

- "Wave XLR not found": check the device names in the log and adjust `InputMatch` / `OutputMatch`.
- "Exclusive open failed": close Wave Link and any app using the Wave XLR, and check the exclusive-mode boxes.
- Clicks/dropouts: set `SafetyPeriods=1` or a bigger `BufferFrames`.

When opening an issue, please attach your `log.txt`.

## Build

Visual Studio 2022 with the "Desktop development with C++" workload:

```
cmake -B build -A x64
cmake --build build --config Release
```

Output: `build\Release\WaveXlrAsio.dll`. The CRT is statically linked, so no redistributable is needed.
It also cross-compiles with MinGW-w64. No Steinberg SDK download is needed: `src/asio_abi.h` declares the public ASIO binary interface.

## Uninstall

Right-click `uninstall.bat` → Run as administrator.

## Limitations

- Wave Link must be closed while using it.
- Windows x64 only.
- Made for the Wave XLR MK2. MK1, Wave XLR Pro or other Elgato devices may work by changing `InputMatch` / `OutputMatch` (reports welcome).

## Disclaimer

Unofficial project, not affiliated with or endorsed by Elgato or Corsair. Use at your own risk.
ASIO is a trademark and software of Steinberg Media Technologies GmbH.

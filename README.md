# sc2d — Steam Controller 2 Android Daemon

Turn your Steam Controller (2026) into a standard Android gamepad, no Steam required.

Works on any **rooted Android device** with `CONFIG_HIDRAW` + `CONFIG_INPUT_UINPUT` (Retroid Pocket 3/4/5, Odin, Ayaneo, etc.).

## What It Does

| Before | After |
|--------|-------|
| Controller only works in Steam | Works in **any** Android app |
| Lizard mode (keyboard/mouse on touchpads) | Real gamepad with buttons, sticks, triggers |
| No rumble | Format documented, needs verification |
| Requires adb to toggle | Quick Settings Tile + controller hotkey |

## Features

- **Full gamepad** — buttons, analog sticks, D-pad, analog triggers
- **Trackpads** — multi-touch via MT protocol
- **Hotkey stop** — press **Steam + Menu + View** 3 times in quick succession → back to lizard mode
- **Quick Settings Tile** — toggle on/off from the notification shade
- **Auto-reconnect** — detects disconnect, reconnects on replug
- **Clean exit** — re-enables lizard mode on shutdown
- **Android Back button** — hold **L5 + R5** for 0.5s to trigger system back

## Requirements

- Root (Magisk recommended)
- Steam Controller 2 + wireless dongle (PID `28de:1304`)
- `adb` for setup

## Quick Start

```bash
# Push the daemon
adb push sc2d /data/local/tmp/
adb shell chmod 755 /data/local/tmp/sc2d

# Run it
adb shell su -c /data/local/tmp/sc2d
```

Press **Steam + Menu + View** 3 times to stop and restore lizard mode.

## Quick Settings Tile

1. Install `sc2d-tile.apk`
2. Swipe down twice → tap the pencil/edit icon
3. Find **SC2D** and drag it to active tiles
4. Tap the tile to start/stop the daemon

## Build from Source

### Daemon

Requires Android NDK (r21e or later):

```bash
export ANDROID_NDK_HOME=/path/to/android-ndk-r21e

cd android
mkdir build && cd build

cmake .. \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-28

make -j$(nproc)
```

### Quick Settings Tile

```bash
cd sc2d-tile
bash build.sh
adb install -r build/sc2d-tile.apk
```

## How It Works

The Steam Controller connects to its dongle via a vendor HID collection (usage page `0xFF00`), sending 54-byte state reports (report ID `0x42`) at ~60 Hz. By default the controller runs in **lizard mode**, emulating a keyboard and mouse so it works without drivers.

The daemon:

1. Opens the vendor hidraw interface
2. Sends `ID_CLEAR_DIGITAL_MAPPINGS` + settings to disable lizard mode
3. Creates a `/dev/uinput` virtual gamepad with standard Android axis mapping
4. Parses the raw `0x42` input reports and emits evdev events
5. Runs a background heartbeat to keep lizard mode suppressed
6. On exit, performs a two-step lizard mode re-enable sequence

The 0x42 report layout is documented in [`SteamController.h`](android/SteamController.h).

## Button Combos

| Combo | Duration | Action |
|-------|----------|--------|
| Tap Steam+Menu+View ×3 | Within 1.5s | Stop daemon, restore lizard mode |
| Hold L5+R5 | 0.5s | Android system Back |

## Rumble

Rumble is **not yet functional**. The intended output report format (`0x80`) has been confirmed by sniffing Steam's USB traffic, but the exact byte values for continuous rumble need verification.

| Report ID | Name | Status |
|-----------|------|--------|
| `0x80` | Haptic Rumble | Format confirmed, needs testing |
| `0x81` | Haptic Pulse | Not implemented |
| `0x82` | Haptic Command | Not implemented |

The captured USB data is in [`android/test/`](android/test/). If you figure out the correct values, PRs are very welcome.

## Input Report Layout

```
Offset  Size  Description
──────  ────  ───────────────────────────
00      1     Report ID (0x42)
01      1     Sequence counter
02      1     Buttons byte 0: A, B, X, Y, RS, Menu, R4
03      1     Buttons byte 1: R5, RB, D-pad, View, LS
04      1     Buttons byte 2: Steam, L4, L5, LB, RS_touch, TP_rt
05      1     Flags: LS_touch, TP_lt, TP_click, Grip_L, Grip_R
06-07   2     Left trigger  (s16 LE, 0–0x7FFF)
08-09   2     Right trigger (s16 LE, 0–0x7FFF)
10-11   2     Left stick X  (s16 LE)
12-13   2     Left stick Y  (s16 LE)
14-15   2     Right stick X (s16 LE)
16-17   2     Right stick Y (s16 LE)
18-19   2     Left trackpad X  (s16 LE)
20-21   2     Left trackpad Y  (s16 LE)
22-23   2     Left trackpad contact area
24-25   2     Right trackpad X (s16 LE)
26-27   2     Right trackpad Y (s16 LE)
28-29   2     Right trackpad contact area
30-31   2     Constant (0x03 0x46)
32-39   8     IMU quaternion (4× s16 LE)
44-45   2     Battery level (0xFFFF = full)
```

## Android Axis Mapping

| SC Input | evdev Code | Android Axis |
|----------|-----------|--------------|
| Left stick X/Y | `ABS_X` / `ABS_Y` | AXIS_X / AXIS_Y |
| Right stick X/Y | `ABS_Z` / `ABS_RZ` | AXIS_Z / AXIS_RZ |
| Left trigger | `ABS_BRAKE` | AXIS_BRAKE |
| Right trigger | `ABS_GAS` | AXIS_GAS |
| D-pad | `ABS_HAT0X` / `ABS_HAT0Y` | AXIS_HAT_X / AXIS_HAT_Y |

## Credits

- [SteamlessController](https://github.com/ddeverill/SteamlessController) — Windows reference implementation (MIT)
- [sc-evdev](https://github.com/Rune580/sc-evdev) — Rust port with unverified rumble
- [SDL](https://github.com/libsdl-org/SDL) — Triton output report definitions (Zlib)

## License

MIT

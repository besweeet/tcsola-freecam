# True Crime: Streets of LA – Free Camera

Short version: I utilized Claude and Cheat Engine to make a freecam + noclip mod for TCSOLA, allowing you to explore inside buildings and parts of the map that are difficult or impossible to visit using existing glitches.

Claude-generated README:

A free-fly camera for the 2003 PC release of **True Crime: Streets of LA**. Toggle it with a key, then fly anywhere on the map with mouse look, including areas the game normally keeps out of reach. The game sees an idle keyboard and mouse while you fly, so your character stays put.

It ships as a single `dinput8.dll` that you drop next to the game executable. There is no injector and nothing is written to the game files on disk.

> **Status:** version 1.0 works for the author on the supported executable, including flights across the entire map without crashes. See [Known limitations](#known-limitations) for what it does not do.

---

## Contents

- [Features](#features)
- [Requirements](#requirements)
- [Installation](#installation)
- [Controls](#controls)
- [Configuration](#configuration)
- [Troubleshooting and the log file](#troubleshooting-and-the-log-file)
- [Known limitations](#known-limitations)
- [How it works](#how-it-works)
- [Reverse-engineering notes](#reverse-engineering-notes)
- [Building from source](#building-from-source)
- [Repository layout](#repository-layout)
- [Version history](#version-history)
- [Disclaimer](#disclaimer)
- [License](#license)

---

## Features

- **Free camera** that starts from your current view and flies in any direction (WASD, up/down, mouse look).
- **Input blocking:** while free cam is on, the game receives no keyboard or mouse input, so the character does not run around. `Esc` still reaches the game so you can pause.
- **Adjustable speed** with fast/slow modifier keys; every key and number is configurable.
- **Crash protection for far-away flying.** The game unloads collision data around the player when the camera is far away, which crashes the stock engine. The mod wraps the game's collision query so a faulting query is aborted instead of crashing the game.
- **Self-diagnosing:** writes a log (`freecam.log`) with camera status, guard activity, and a full crash report (faulting address, registers, likely return addresses) if something still goes wrong.
- **Version-checked:** every patch verifies the original bytes first, so on an unsupported executable the mod does nothing instead of corrupting it.

## Requirements

- Windows (32-bit game; works on 64-bit Windows).
- The PC executable **`TrueCrime.exe` with MD5 `8663533808a653d81049a08b8df3b241`** (image base `0x400000`, Direct3D 8).
  Other builds have different addresses. The mod detects this and stays inactive (see the log).
- A legitimately obtained copy of the game. No game files are included here.

Check your executable's hash with PowerShell:

```powershell
Get-FileHash .\TrueCrime.exe -Algorithm MD5
```

## Installation

1. Download the latest release and extract it.
2. Copy **`dinput8.dll`** and **`freecam.ini`** into the game folder, next to `TrueCrime.exe`.
3. Start the game. After the first launch a `freecam.log` appears next to the DLL.

To uninstall, delete `dinput8.dll` (and optionally `freecam.ini` / `freecam.log`).

> **Antivirus note:** a proxy DLL that hooks game code can trigger heuristic detections, the same way any mod does. The complete source is in this repository so you can inspect and build it yourself.

> **Windowed mode / Alt-Tab:** the original game is fixed at 640×480 and tends to crash when you Alt-Tab back in. That is a quirk of the game, not of this mod. A D3D8 wrapper such as dgVoodoo2 fixes it, but it uses the same `d3d8.dll` slot a graphics hook would, so keep this in mind if you chain other tools.

## Controls

Defaults (all changeable in `freecam.ini`):

| Action | Key |
|---|---|
| Toggle free cam | `F9` |
| Look | Mouse (arrow keys also work) |
| Forward / back | `W` / `S` |
| Strafe left / right | `A` / `D` |
| Up / down | `E` / `Q` |
| Faster / slower | `Shift` / `Ctrl` |
| Pause the game | `Esc` (still passed through) |

When you press `F9` the free camera starts exactly where the game camera was. Press it again and the game camera takes over immediately.

## Configuration

Edit `freecam.ini` next to the DLL. It is read once at game start, so restart the game after changing it.

Keys are Windows [virtual-key codes](https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes) in decimal (letters are their ASCII code, e.g. `W` = 87, `F9` = 120).

| Setting | Default | Meaning |
|---|---|---|
| `ToggleKey` | `120` (F9) | Toggle free cam |
| `Forward`, `Back`, `Left`, `Right` | `87, 83, 65, 68` | Movement keys |
| `Up`, `Down` | `69, 81` | Vertical movement |
| `Fast`, `Slow` | `16, 17` | Speed modifiers (Shift, Ctrl) |
| `Speed` | `15` | Movement speed in game units per second |
| `FastMultiplier` / `SlowMultiplier` | `5` / `0.2` | Applied while the modifier key is held |
| `UseMouse` | `1` | Mouse look on/off |
| `MouseSensitivity` | `0.0025` | Radians per mouse count |
| `InvertMouseX` / `InvertMouseY` | `0` / `0` | Flip a look axis if it feels backwards |
| `BlockGameInput` | `1` | Hide keyboard/mouse from the game while free cam is on |
| `MaxDistance` | `0` | Keep the camera within N units of the player (`0` = unlimited). See [Known limitations](#known-limitations). |
| `QueryGuard` | `1` | Wrap the collision query so a faulting query is aborted instead of crashing |
| `CrashGuard` | `1` | Additional fallback guard for a known faulting matrix helper |
| `ForwardRow` | `-1` | Advanced: force which camera matrix row is "forward" (`-1` = auto) |
| `ForwardSign` | `0` | Advanced: force forward direction (`1`/`-1`, `0` = auto) |
| `Log` | `1` | Write `freecam.log` |

## Troubleshooting and the log file

`freecam.log` is appended to on every launch. Useful lines:

| Log line | Meaning |
|---|---|
| `camera hook installed` | The camera patch is active. If missing, the executable did not match (check the MD5). |
| `query guard installed` | Collision-query try/catch is active. |
| `DirectInput device created: … type=1` / `type=2` | Keyboard / mouse recognized, so input blocking and mouse look work. If you do not see both, keys will also move the character. |
| `free cam ON … fi= ui= ri= sF= sU= sR=` | Detected camera axes at activation. Send this line if movement or look directions are wrong. |
| `status: cam=(…) dist-from-player=… free-virtual=…MB` | Printed every 2 seconds while active. |
| `query guard: aborted a collision query …` | A faulting collision query was safely aborted. Expect many of these when flying far from the player. |
| `=== UNHANDLED CRASH` | A real crash. The lines after it (address, registers, return addresses) are what is needed to diagnose it. |

Common fixes:

| Symptom | Fix |
|---|---|
| Nothing happens on `F9` | Check the log for `camera hook installed`; make sure the game window is focused. |
| Character also moves when flying | No `type=1` line in the log; set `BlockGameInput=1` and check the log. |
| Mouse look inverted | `InvertMouseY=1` (and/or `InvertMouseX=1`). |
| Strafe, turn, or forward is mirrored | Send the `free cam ON` log line. `ForwardRow` / `ForwardSign` can override detection. |
| Unstable in a particular area | Set `MaxDistance` (for example `400`) as a stopgap and send the log. |
| Game will not start | Set `QueryGuard=0` to disable the collision-query wrapper and send the log. |

## Known limitations

- **Far-from-player flights can leave the player in an invalid state.** Once the camera is roughly 400–500 or more units from the player, the game has unloaded the collision data around the player, and the player's own collision queries start faulting; the guard aborts them every frame (the log shows many `query guard` lines). In a recorded session the logged player position then became invalid (about ±1e38). Exploring and flying is unaffected, but what happens when you switch free cam **off** after such a flight has not been tested. The safe workflow for far exploration is to fly, then quit or reload a save. If you want to return to normal play, set `MaxDistance` to about `400` (a distance of about 527 units was survived without issue during testing).
- **Street-name display and traffic follow the player, not the camera.** In testing, street names did not update with the camera, and pedestrians appeared near the camera while vehicles did not. The player stays where they stand, so anything keyed to the player's position stays there. Making the player follow the camera has not been attempted.
- **The world streams around the camera.** Far from the player, the data around the *player* is unloaded; this is the underlying cause of the stock-engine crashes the guard works around.
- **One executable only.** Patches are tied to the MD5 above.
- Mouse look relies on hooking DirectInput; if a device is not recognized, use the arrow keys.
- Windows only; there is no Linux/Proton testing.

## How it works

```
 TrueCrime.exe ──imports──> DINPUT8.dll   (our proxy, loaded from the game folder)
                                │
        ┌───────────────────────┼─────────────────────────────────────┐
        │ DllMain               │ DirectInput8Create                  │
        │  • verify exe bytes   │  • forward to the real dinput8.dll  │
        │  • patch 2 functions  │  • patch the returned object's      │
        │  • crash logging      │    vtable: CreateDevice →           │
        └───────────────────────┘    GetDeviceState / GetDeviceData   │
                                     (blank input while free cam on, │
                                      harvest mouse deltas)          │
```

**1. Camera override.** The game has a generic 4×4 matrix copy (`0x4010A0`, ~227 callers). The camera node's transform reaches the renderer through it. The mod diverts that function and compares the destination with the current camera node's matrix. For any other matrix it runs the original code; for the camera it writes its own matrix. Every other consumer of the camera node (culling, draw distance) sees the free camera.

**2. Axis detection.** The matrix layout was not known at build time, so at activation the mod works out which of the three orientation rows is forward (the row best aligned with the direction to the player, because the stock chase camera always looks at the player), which is up (largest Y), and which is right (the other). If the player is almost on top of the camera, the standard layout is assumed. It then builds yaw/pitch from that and writes the rows back. The math is a pure header (`freecam_math.h`) with a native unit test that round-trips 4,032 combinations of row order and sign.

**3. Input blocking.** The proxy forwards `DirectInput8Create` and patches the returned object's `CreateDevice` entry in place. Keyboard and mouse devices are recognized (by GUID, falling back to `GetDeviceInfo`) and their `GetDeviceState` / `GetDeviceData` are patched. While free cam is on, keyboard state is zeroed (except `Esc`) and mouse movement is consumed for looking, then zeroed.

**4. Collision-query try/catch.** The game's collision-world query (`0x524580`) is wrapped with an assembly stub that saves the callee-saved registers and diverts the return address through a thunk. A vectored exception handler catches access violations that occur inside it, restores the registers, rewrites the stack, resets the x87 stack and returns the game's "no hit" value. A second, narrower fallback guard skips two vector-times-matrix helpers if they fault outside a wrapped query.

All hooks are 5-byte `jmp` detours installed once at load. Each one was chosen after statically verifying that no jump lands inside the bytes being replaced. The mod never modifies files on disk.

## Reverse-engineering notes

Findings for anyone modding this build (MD5 `8663533808a653d81049a08b8df3b241`; static analysis and Cheat Engine, no symbols). Addresses are virtual addresses; the image has no ASLR.

| Address | What it is |
|---|---|
| `0x6D9570` | Pointer to the **current camera node** (getter `0x4E4880`, setter `0x4E4850`; default camera object at `0x7076C0`) |
| `0x6D9578` | Pointer to the **player / controlled node** (follow `+0x58` parent pointers to the root; when in a car the root is the car) |
| node `+0x10` | 4×4 transform: three orientation rows, position row at `+0x40` (Y is up) |
| node `+0x58` | Parent node pointer |
| `0x4010A0` | Matrix4 copy (`dest = ecx`, `src = [esp+4]`, `ret 4`); the camera hook point |
| `0x524580` | **Collision-world query** (`this = 0x70CE40`, 3 stack args, `ret 0xC`, float result in ST0; no-hit value is the constant at `0x67FF90` ≈ −1e38; the game's own no-hit exit also stores the end point at `result+0x30`) |
| `0x4EFAE0` | **Character controller** (not hooked in this release): five collision probes per character per frame, results around object `+0x13C0` / `+0x1500` / `+0x18A0`; `ret 8`, float in ST0 |
| `0x54BDD0`, `0x54BE70` | Vector-times-matrix helpers (a known fault site when collision data is stale) |
| `0x6D95F1`, `0x6D95F2` | Byte flags that enable the built-in **debug overlay** (FPS, memory, `Pos =`, `Camera =`); the printing function is at `0x4E59E0` |
| `Data\TrueCrime.ini` | Per-time-of-day environment presets (`[day]`, `[dawn]`, `[evening]`, `[night]`, `[rainynight]`) with `near` / `far` pairs, scaled by 0.3125 on load. Not tested as a draw-distance control. |

Other facts:

- 32-bit PE, Direct3D 8 and DirectInput 8, with an unidentified self-decrypting stub in a section named `revolt` at the entry point. `.text` itself is not encrypted.
- Position and collision are separate: the player's node transform is an **output** of the character controller, so editing it alone does not move the character.
- World streaming and culling follow the **camera**; the player's collision data is unloaded when the camera leaves.

## Building from source

Requirements: `mingw-w64` with the 32-bit target (`i686-w64-mingw32-gcc`) and a native `gcc` for the math test.

```sh
sudo apt install gcc-mingw-w64-i686      # Debian / Ubuntu
sh build.sh
```

`build.sh` first compiles and runs the native math test (`src/test_math.c`), then cross-builds `dinput8.dll`. The DLL is built without SSE (`-mno-sse -mno-mmx`) on purpose, so the hooks never disturb the game's `xmm` registers, and with `-Wl,--kill-at` so the export is the undecorated `DirectInput8Create` the game imports.

## Repository layout

```
.
├── README.md
├── freecam.ini          default configuration
├── build.sh             test + cross-compile
└── src/
    ├── dllmain.c        proxy DLL: hooks, input blocking, guards, logging
    ├── freecam_math.h   pure camera math (axis detection, yaw/pitch, movement)
    └── test_math.c      native unit test for the math
```

Prebuilt binaries are best distributed through GitHub Releases rather than committed to the repository.

## Version history

| Version | Changes |
|---|---|
| 1.0 | First release. Free camera with mouse look, DirectInput input blocking, automatic camera-axis detection, collision-query crash protection, configurable keys/speeds, and a diagnostic log with crash reports. |

## Disclaimer

This is an unofficial fan modification and is not affiliated with or endorsed by Activision, Luxoflux, or any rights holder. It contains no game assets and does not circumvent copy protection; it requires your own legitimately obtained copy of the game. Use at your own risk and keep a backup of your save data.

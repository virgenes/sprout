# Sprout 🌱

[Español](readme_spanish.md) · **English**

**Sprout** is a complete desktop solution for running *Plants vs. Zombies 2* on PC. It combines an ARM emulation layer (the engine) with a full-featured graphical launcher — controls editor, gamepad support, OpenGL 2.0 rendering, and more — all in one package.

---



## Table of Contents

- [Features](#features)
- [Architecture](#architecture)
- [Launcher](#launcher)
- [Controls](#controls)
- [Supported versions](#supported-versions)
- [Quick start](#quick-start)
- [Building](#building)
- [Project structure](#project-structure)
- [Credits](#credits)
- [License](#license)

---

## Features

### Launcher

- **Visual controls editor** — remap every game action to any keyboard key or gamepad button through a clean GUI
- **Gamepad support** — Xbox, PlayStation, and generic controllers; live input detection; per-action mapping with dead zone and axis configuration
- **Language selector** — set the game's internal locale (`en_US`, `es_ES`, etc.)
- **Emulated IAP** — toggle simulated purchase responses for testing
- **Persistent config** — all settings saved to `config.ini`, generated automatically on first launch
- **Game path configuration** — point to your `libPVZ2.so` and `.obb` through the UI

### Engine

- **JIT CPU emulation** — [dynarmic](https://github.com/dynarmic/dynarmic) translates ARM32 instructions to x86_64 in real time
- **Android API reimplementation** (Wine-style) — OpenGL ES → OpenGL 2.0, OpenSL ES → SDL audio, fake JNI/JavaVM for Java calls
- **Guest memory manager** — secure heap with quarantine zone for use-after-free detection
- **Virtual file system** — maps Android paths to PC locations, reads `.obb` assets (RSB/RSG format)
- **Multi-version support** — byte fingerprints detect the game version and load correct symbol offsets automatically

### Graphics & Input

- **OpenGL 2.0** — runs on virtually any GPU
- **Window resizing** with automatic scaling and click remapping
- **Frame limiter** for consistent game speed
- **Full keyboard + mouse mapping** — every game action configurable
- **Gamepad hot-plugging** — connect and disconnect without restart

---

## Architecture

The original codebase was a **monolith** — game addresses, runtime logic, and system call handlers were intertwined. Supporting a new game version meant hunting through the entire codebase for hardcoded offsets.

Sprout refactored everything into an **orchestrator architecture** with a strict layering rule:

> **Only one layer may contain concrete addresses from the `.so`.**
> The rest operate exclusively through that layer.

### Layer diagram

```
┌──────────────────────────────────────────────────────────┐
│                     LAUNCHER (GUI)                        │
│  controls editor · gamepad config · IAP toggle · locale   │
└────────────────────────┬─────────────────────────────────┘
                         │ launches
                         ▼
┌──────────────────────────────────────────────────────────┐
│  ENGINE (orchestrator)                                    │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐               │
│  │  engine/ │  │  game/   │  │  dex/    │               │
│  │  boot    │  │ symbols  │  │  JNIEnv  │               │
│  │  frame   │──│ (addrs!) │──│  hooks   │               │
│  │  lifecycle│  └──────────┘  └──────────┘               │
│  └──────────┘                                            │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐               │
│  │runtime/  │  │dependenc.│  │diagnostic│               │
│  │JIT, heap │  │libc, GLES│  │probes    │               │
│  └──────────┘  └──────────┘  └──────────┘               │
└──────────────────────────────────────────────────────────┘
```

### What each layer does

| Layer | Responsibility | Holds `.so` addresses? |
|---|---|---|
| [runtime/](pvz2native/src/runtime/) | JIT execution, guest heap, threads, synchronization, RSB cache | **Never** |
| [game/symbols.cpp](pvz2native/src/game/symbols.cpp) | Symbol table — maps function names to addresses per version | **The only one** |
| [engine/](pvz2native/src/engine/) | Boot sequence, lifecycle, frame loop — calls symbols through `sym()` | Only via `sym()` |
| [dependencies/](pvz2native/src/dependencies/) | Handlers for every Android `.so` (libc, libm, GLES, OpenSL ES, libz, libdl, libstdcxx, liblog) | No |
| [dex/](pvz2native/src/dex/) | Fake `JNIEnv`/`JavaVM` + one file per hooked Java class | No |
| [diagnostics/](pvz2native/src/diagnostics/) | Watchpoints, PC sampling, guest probes | Only via `sym()` |

### Why this matters

Adding support for a new game version requires **exactly one change**: a new entry in `kVersions` inside `symbols.cpp`. The engine, runtime, dependencies, and hooks remain untouched.

The engine orchestrates the boot without knowing where anything lives in memory. Dependencies are registered by name and dispatched automatically. The launcher drives the whole thing through config files — no recompilation needed to switch game versions.

---

## Launcher

The launcher (`sprout.exe`) provides a full graphical interface for configuring and running the game:

- **Controls tab** — visual grid showing every game action mapped to your keyboard/gamepad. Click any entry to rebind. Swap between keyboard and gamepad layouts.
- **Gamepad tab** — configure stick dead zones, trigger thresholds, invert axes, and test input live
- **Graphics tab** — window mode, resolution, frame limiter settings
- **Language tab** — pick the game's display language from a dropdown
- **IAP toggle** — flip a switch to enable/disable emulated in-app purchases
- **Paths tab** — browse for your `libPVZ2.so` and `.obb` files

All settings persist in `config.ini`, fully documented with inline comments.

---

## Controls

### Default keyboard layout

| Action | Key |
|---|---|
| Tap / select | Left click |
| Drag / swipe | Left click + move |
| Type text | Keyboard (when a text field is open) |
| Confirm | `Enter` |
| Delete / Back | `Backspace` |
| Pause | `Escape` |
| Place plant (slot 1-8) | `1`–`8` |
| Speed up | `Space` |

### Gamepad

- All actions mappable through the launcher's controls editor
- Left stick → drag, Right stick → scroll (configurable)
- Face buttons → plant slots, triggers → confirm/cancel
- Start → pause, Select → speed up

Every binding is customizable. The launcher detects your controller model and applies sensible defaults.

---

## Supported versions

The game version is detected by **byte fingerprint** at two known native functions. If no fingerprint matches, boot is refused rather than running blind.

| Version | OBB | Status |
|---|---|---|
| **1.6.10** (2013) | `main.7.com.ea.game.pvz2_na.obb` | Boots to menu |
| **4.5.2** (2016) | `main.147.com.ea.game.pvz2_row.obb` | Boots to menu |

To add a version: add one entry to `kVersions` in [symbols.cpp](pvz2native/src/game/symbols.cpp).

---

## Quick start

1. Download the latest release from the [Releases](https://github.com/virgenes/sprout/releases) page
2. Extract `Sprout_v1.0.zip` to a folder of your choice
3. Run `sprout.exe`
4. The launcher opens — the `.obb` is created automatically on first launch

---



## Building

### Requirements

- **Windows x64**
- **MinGW-w64** (GCC with C++20 support)
- **CMake** ≥ 3.16

### Compile

```bat
compile.bat
```

> The `.bat` file has toolchain paths hardcoded to the author's machine. Adjust `CMAKE_C_COMPILER`, `CMAKE_MAKE_PROGRAM`, `BOOST_ROOT`, and `Python_EXECUTABLE` to match your setup, or invoke CMake manually:

```bash
mkdir build && cd build
cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
```

The binaries land in `build/sprout/`.

---

## Project structure

```
Sprout/
├── pvz2native/               ← engine + launcher source
│   ├── include/sprout/       ← public headers
│   ├── src/                  ← engine implementation
│   │   ├── runtime/          ← JIT, guest heap, threads
│   │   ├── dependencies/     ← Android .so reimplementations
│   │   ├── dex/              ← fake JNI/JavaVM + class hooks
│   │   ├── engine/           ← boot, lifecycle, frame loop
│   │   ├── game/             ← symbols (the only layer with addresses)
│   │   ├── diagnostics/      ← watchpoints, probes
│   │   ├── gfx/              ← OpenGL ES → OpenGL translation
│   │   ├── audio/            ← OpenSL ES → SDL audio
│   │   ├── input/            ← keyboard, mouse, gamepad
│   │   ├── patch/            ← runtime code patching
│   │   └── elf32/            ← ELF loader
│   ├── launcher/             ← GUI (controls editor, config, etc.)
│   └── tests/                ← unit tests
├── CMakeLists.txt
├── compile.bat
└── README.md
```

---

## Credits

- **[Maximo](https://www.pvz2.app/)** — original insight and foundational research that made this project possible
- **[Optijuegos](https://www.youtube.com/@Optijuegos)** — community support and beta testing

---

## License

Code is **MIT** (see [LICENSE](LICENSE)). Third-party dependencies (dynarmic, SDL, zlib, glad, stb) carry their own licenses.

*Plants vs. Zombies 2* and all related assets are trademarks of **Electronic Arts / PopCap Games**. This project is **not affiliated with, endorsed by, or sponsored by** EA or PopCap.

---

<p align="center">
  <b>Sprout</b> — Growing PvZ2 on PC, one native call at a time
</p>

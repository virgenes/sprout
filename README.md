# Sprout 🌱

[Español](readme_spanish.md) · **English**

**Sprout** is a complete desktop solution for running *Plants vs. Zombies 2* on PC. It combines an ARM emulation layer (the engine) with a full-featured graphical launcher — controls editor, gamepad support, OpenGL 2.0 rendering, software renderer (Mesa3D), system diagnostics, and much more — all in one package.

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

### Launcher (V2 Modern Redesign)

- **System Diagnostics Scanner** — automatic background scan on startup checking GPU OpenGL capabilities, Mesa3D software renderer files, VC++ Runtimes, RAM memory, disk space, and CPU SIMD (AVX2/SSE4.1) support
- **Automated Downloads & Installers (WinHTTP)** — one-click integrated download manager with multi-hop redirect support to automatically fetch and set up missing dependencies (Mesa3D software rendering package, VC++ Redistributable)
- **Locate / Import Mesa3D (📁 Folder Button)** — native Windows file dialog to locate and import existing `opengl32.dll` binaries, Mesa folders, or `.7z`/`.zip` packages directly
- **Low-End PC Optimization** — built with lightweight Win32 GDI custom-painted cards and sub-millisecond execution, designed to run smoothly on legacy or ultra-low-spec hardware without window flickering (`WS_CLIPCHILDREN`)
- **Visual controls editor** — remap every game action to any keyboard key or gamepad button through a clean GUI
- **4-Tab Navigation Interface** — seamless navigation between **Home**, **System Diagnostics**, **Game Options**, and **Engine Settings**
- **Software Renderer Toggle (Mesa3D)** — switch between Hardware (OpenGL GPU) and Software (CPU via Mesa3D LLVMpipe) rendering for PCs lacking dedicated graphics
- **MSAA Anti-Aliasing Control** — toggle Off, 2x, 4x, or 8x Multisample Anti-Aliasing directly from the Engine tab
- **Gamepad support** — Xbox, PlayStation, and generic controllers; live input detection and action mapping
- **Language & Features Selector** — choose locale (`en_US`, `es_ES`, etc.), toggle emulated IAPs, V-Sync, persistent saves, and debug console

### Engine

- **CPU JIT Emulation** — [dynarmic](https://github.com/dynarmic/dynarmic) translates ARM32 instructions to x86_64 in real-time
- **Android Re-implementation** (Wine style) — OpenGL ES → OpenGL 2.0, OpenSL ES → SDL audio, dummy JNI/JavaVM for Java calls
- **Automatic Signature Scanner** — pattern matching engine (`game_symbols_pattern_scan`) auto-discovers function offsets in unlisted `libPVZ2.so` binaries
- **Native Guest Hooking Framework** — safe hook API (`hook_framework`) to register C++ callbacks at game ARM function addresses for modding
- **Vectored Exception Handler (VEH)** — low-overhead page fault interception for fastmem optimization on Windows
- **Guest Memory Manager** — safe heap with quarantine zone to catch use-after-free
- **Virtual File System** — maps Android paths to PC locations, reads assets from `.obb` (RSB/RSG format)
- **Multi-version Support** — byte fingerprints and signature scanner detect game versions automatically

### Graphics and Input

- **Hardware & Software Renderers** — OpenGL 2.0 GPU acceleration or Mesa3D CPU software rendering
- **MSAA Anti-Aliasing** — configurable multisample antialiasing hints (`gl_msaa`) in `config.ini`
- **Resizable Window** with automatic scaling and click remapping
- **Frame Limiter** for consistent gameplay speed
- **Full Keyboard + Mouse Mapping** — every game action configurable
- **Gamepad Hotplugging** — connect and disconnect controllers on the fly without restarting

---

## Architecture

The original code was a **monolith** — game addresses, runtime logic, and syscall handlers were intertwined. Supporting a new version meant hunting for hardcoded offsets across the entire codebase.

Sprout refactored it into an **orchestrator architecture** with a strict layering rule:

> **Only one layer may contain concrete `.so` addresses.**
> Everything else operates exclusively through that layer.

### Layer Diagram

```
┌──────────────────────────────────────────────────────────┐
│                    LAUNCHER (GUI)                        │
│  controls editor · gamepad · IAP · locale                │
└────────────────────────┬─────────────────────────────────┘
                         │ launches
                         ▼
┌──────────────────────────────────────────────────────────┐
│  ENGINE (orchestrator)                                    │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐               │
│  │ engine/  │  │  game/   │  │  dex/    │               │
│  │ boot     │  │ symbols  │  │  JNIEnv  │               │
│  │ frame    │──│ (addrs)  │  │  hooks   │               │
│  │ lifecycle│  └──────────┘  └──────────┘               │
│  └──────────┘                                            │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐               │
│  │runtime/  │  │dependenc.│  │diagnostic│               │
│  │JIT, heap │  │libc, GLES│  │probes    │               │
│  └──────────┘  └──────────┘  └──────────┘               │
└──────────────────────────────────────────────────────────┘
```

### Layer Responsibilities

| Layer | Responsibility | Has `.so` addresses? |
|---|---|---|
| [src/runtime/](src/runtime/) | JIT execution, guest heap, threads, sync, VEH handler, RSB cache | **Never** |
| [src/game/symbols.cpp](src/game/symbols.cpp) | Symbol table — maps function names to addresses per version | **The only one** |
| [src/engine/](src/engine/) | Boot sequence, lifecycle, frame loop — calls symbols via `sym()` | Only via `sym()` |
| [src/dependencies/](src/dependencies/) | Handlers for each Android `.so` (libc, libm, GLES, OpenSL ES, libz, libdl, libstdcxx, liblog) | No |
| [src/dex/](src/dex/) | Dummy `JNIEnv`/`JavaVM` + one file per hooked Java class | No |
| [src/diagnostics/](src/diagnostics/) | Watchpoints, PC sampling, guest probes | Only via `sym()` |

### Why It Matters

Adding support for a new game version requires **exactly one change**: a new entry in `kVersions` inside `symbols.cpp`. The engine, runtime, dependencies, and hooks remain untouched.

The engine orchestrates boot without knowing where anything lives in memory. Dependencies register by name and dispatch automatically. The launcher handles everything through config files — no recompilation needed to switch versions.

---

## Launcher

The launcher (`launcher.exe`) provides a modern Win32 native GUI designed for maximum performance and minimum memory footprint on any PC:

- **Home Tab** — System status badge, recommended configuration (Hardware vs Software), and launch button.
- **System Tab** — Real-time diagnostic panel covering OpenGL, Mesa3D, VC++ Runtime, RAM, Disk Space, Windows OS, and AVX2/SSE4.1 SIMD support, along with one-click download buttons and a **📁 Folder import button** for Mesa3D.
- **Options Tab** — Window/fullscreen mode, FPS limiter (30, 60, 120, 144, uncapped), graphics quality, shadows, render scale, language, and feature toggles (V-Sync, emulated IAP, persistent saves, console).
- **Engine Tab** — Renderer selector (Hardware OpenGL vs Software CPU Mesa3D) and MSAA Anti-Aliasing settings.
- **Controls Dialog** — Complete remapping modal for keyboard and gamepad with live key capture.

All settings are stored in `config.ini`, fully documented with inline comments.

---

## Controls

### Default Keyboard Layout

| Action | Key |
|---|---|
| Touch / select | Left click |
| Drag / swipe | Left click + move |
| Type text | Keyboard (when text field is focused) |
| Confirm | `Enter` |
| Back / Erase | `Backspace` |
| Pause | `Escape` |
| Plant slot (1–8) | `1`–`8` |
| Fast-forward | `Space` |

### Gamepad

- All actions remappable from launcher controls editor
- Left stick → drag, Right stick → scroll (configurable)
- Face buttons → plant slots, Triggers → confirm/cancel
- Start → pause, Select → fast-forward

Every combination is customizable. The launcher detects your controller model and applies sensible defaults.

---

## Supported Versions

Game version is detected using **byte fingerprints** on two known native functions. If neither matches, launch is rejected instead of running blindly.

| Version | OBB | Status |
|---|---|---|
| **1.6.10** (2013) | `main.7.com.ea.game.pvz2_na.obb` | Boots to menu |
| **4.5.2** (2016) | `main.147.com.ea.game.pvz2_row.obb` | Boots to menu |

To add a version: add an entry to `kVersions` in [symbols.cpp](src/game/symbols.cpp).

---

## Quick Start

1. Download the latest release from the [Releases](https://github.com/virgenes/sprout/releases) page
2. Extract `Sprout_PvZ2_PC.zip` to any folder
3. Run `launcher.exe`
4. The launcher opens — check system diagnostics or click **Launch Game** (`sprout.exe`)

---

## Building

### Prerequisites

- **Windows x64**
- **MSVC 2019/2022** or **MinGW-w64** (C++20 support)
- **CMake** ≥ 3.16

### Build Commands

Using MSVC (Visual Studio 2019 / 2022):

```bat
compile-msvc.bat
```

Or using CMake manually:

```bash
mkdir build-win32 && cd build-win32
cmake -G "Visual Studio 16 2019" -A x64 ..
cmake --build . --config Release
```

Output executables:
- `build-win32/Release/launcher.exe` — Launcher GUI
- `build-win32/pvz2native/Release/sprout.exe` — Game engine

---

## Project Structure

```
Sprout/
├── include/sprout/          ← Public C/C++ header files
│   ├── audio/               ← OpenSL ES / SDL audio headers
│   ├── dependencies/        ← Android system library headers
│   ├── dex/                 ← Dummy JNIEnv & Java class hooks
│   ├── diagnostics/         ← Watchpoint & probe headers
│   ├── engine/              ← Boot & lifecycle headers
│   ├── game/                ← Symbol table declarations
│   ├── gfx/                 ← OpenGL / MSAA / frame limiter headers
│   ├── input/               ← Gamepad & keyboard headers
│   ├── patch/               ← Code detours & patching
│   └── runtime/             ← JIT, heap, VEH, VEH handler & hook framework
├── src/                     ← Core engine implementation
│   ├── audio/               ← Audio output & sound repository
│   ├── dependencies/        ← Android .so replacements (libc, GLES, zlib)
│   ├── dex/                 ← Java VM & class hook implementations
│   ├── engine/              ← Boot sequence, frame loop, lifecycle
│   ├── game/                ← symbols.cpp (the ONLY layer with addresses)
│   ├── gfx/                 ← OpenGL translation layer
│   ├── input/               ← Gamepad thread & input queue
│   ├── patch/               ← Runtime code patching
│   └── runtime/             ← Dynarmic JIT wrapper, guest heap, VEH, hook framework
├── launcher/                ← Win32 GUI Launcher (main.cpp, scanner.cpp, widgets)
├── mesa/                    ← Software rendering Mesa3D binaries & helpers
├── tools/                   ← Ghidra RE scripts & RSB/RTON modding tools
├── tests/                   ← Unit tests
├── CMakeLists.txt           ├── Root CMake build script
├── compile-msvc.bat         ├── MSVC 2019/2022 build script
├── readme_spanish.md        ├── Documentación en Español
└── README.md                └── Main documentation (English)
```

---

## Credits

- **[Maximo](https://www.pvz2.app/)** — original idea and foundational research that made this project possible
- **[Optijuegos](https://www.youtube.com/@Optijuegos)** — community support, testing, and concept direction

---

## License

Code is licensed under the **MIT License** (see [LICENSE](LICENSE)). Third-party dependencies (dynarmic, SDL, zlib, glad, stb) have their respective licenses.

*Plants vs. Zombies 2* and all related assets are registered trademarks of **Electronic Arts / PopCap Games**. This project is **not affiliated with, endorsed by, or sponsored by** EA or PopCap.

---

<p align="center">
  <b>Sprout</b> — Cultivating PvZ2 on PC, one native call at a time
</p>

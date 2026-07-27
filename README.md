# PvZ2Native

📖 [Español](readme_spanish.md) · **English**

**Runs the real native library of *Plants vs. Zombies 2* for Android (`libPVZ2.so`, ARM32) on PC.** The only thing that is emulated is the **CPU**: PvZ2 exists only compiled for ARM (there is no x86/x64 build), so a JIT translates its ARM instructions to x86_64. Everything else — Android, JNI, libc, OpenGL ES, the filesystem — **is not emulated, it is reimplemented natively**, the same way **Wine** does with Windows.

> [!WARNING]
> Experimental project under active development. It boots as far as the main-menu load; rendering, audio and input are work in progress. **The game is not included**: you must supply your own `libPVZ2.so` and your own `.obb`.

---

## 📖 What it is (and what it is NOT)

It is not a *port* of the game's source code. It is a **hybrid of two techniques**, and the distinction matters:

- **Only the CPU is emulated.** The game binary is ARM machine code and PvZ2 was never compiled for x86/x64, so there is no way to run it directly on a PC processor. A JIT (dynarmic) reads those ARM instructions and translates them to x86_64 on the fly. This is the only "emulated" part.
- **Android is NOT emulated: it is reimplemented.** When the game calls an Android function (open a file, draw with OpenGL ES, allocate memory, invoke Java…), that call is serviced by **native PC code** that does the real work using the equivalent desktop API. There is no virtual Android running underneath; there is a **reimplementation** of the functions the game needs.

This is exactly **Wine's** approach — *"Wine Is Not an Emulator"* — taken one step further: Wine reimplements Windows calls without emulating anything because `.exe` files are already x86; here, because the game is ARM, the CPU has to be emulated as well. The rest of the philosophy is identical: **translate calls, don't simulate a whole machine.**

```text
┌──────────────────────────────────────────────┐
│   libPVZ2.so  (ELF ARM32, EABI5, untouched)   │   ← the real Android game
└───────────────────────┬──────────────────────┘
                        │  ARM instructions
                        ▼
┌──────────────────────────────────────────────┐
│   dynarmic  —  JIT ARM32 → x86_64             │   ← ONLY emulated component
└───────────────────────┬──────────────────────┘      (the CPU only)
                        │  "Android" calls
                        ▼
┌──────────────────────────────────────────────┐
│   Compatibility layer (this project)          │   ← NATIVE reimplementation,
│                                               │      Wine-style (nothing emulated)
│   Fake JNI / JavaVM    ·   libc / libm / libz │
│   OpenGL ES → OpenGL   ·   OpenSL ES → SDL    │
│   Own GuestHeap        ·   VFS (paths + .obb) │
└───────────────────────┬──────────────────────┘
                        │
                        ▼
┌──────────────────────────────────────────────┐
│   Windows x64  ·  SDL2  ·  OpenGL 2.0 (glad)  │
└──────────────────────────────────────────────┘
```

When the game asks the operating system for something (open a file, allocate memory, draw, play a sound, call into Java), that call leaves the emulated code as a trap and is serviced by a *handler* in this layer, backed by the equivalent PC API.

---

## 🧩 Architecture

The code lives in [pvz2native/](pvz2native/) and is split into layers under one strict rule: **only one layer may contain concrete addresses from the `.so`.** That is what makes supporting another game version a matter of adding data, not rewriting logic.

| Layer | Question it answers | May it hold a `.so` address? |
| --- | --- | :---: |
| [runtime/](pvz2native/src/runtime/) | How to run ARM code (JIT, heap, threads, synchronization) | **Never** |
| [game/symbols.cpp](pvz2native/src/game/symbols.cpp) | Where each thing lives in *this* binary | **The only one that may** |
| [engine/](pvz2native/src/engine/) | What to call and in what order (boot, lifecycle, frame) | Only via `sym()` |
| [dependencies/](pvz2native/src/dependencies/) | How to answer each Android `.so` (libc, libm, GLES, …) | No |
| [dex/](pvz2native/src/dex/) | How to answer the Java side (fake JNIEnv + per-class hooks) | No |
| [diagnostics/](pvz2native/src/diagnostics/) | How to interrogate the guest (watchpoints, PC sampling) | Only via `sym()` |

Notable pieces:

- **JIT:** [dynarmic](dynarmic/) (A32 frontend only). One JIT per guest thread, with an identity *page table* over the `.so`'s memory for speed.
- **Native dependencies:** one module per Android `.so` the game links against. libc is split by header (`string`, `stdio`, `stdlib`, `time`, `pthread`, `locale`, `ctype`, …), plus `libm`, `libz`, `liblog`, `libgles`, `libstdcxx`, `libopensles`, `libdl`. Each handler is a `void(GuestCall&)` registered by name.
- **Java side:** [jni_env.cpp](pvz2native/src/dex/jni_env.cpp) implements a fake `JNIEnv`/`JavaVM` (objects, strings, arrays, references); [dex/hooks/](pvz2native/src/dex/hooks/) has one file per Java class (`AndroidGameApp`, `AndroidSurfaceView`, HTTP, Google Play, Facebook…).
- **Graphics:** the game's GLES calls are translated to **OpenGL 2.0 (compatibility profile)** via `glad`; SDL provides the window and GL context, playing the role of Android's `GLSurfaceView`.
- **Resources:** a VFS translates Android paths to PC paths and reads assets from the game's real `.obb` (RSB/RSG format).

---

## 🎮 Supported game versions

The version is detected by a **byte fingerprint** at two known native functions; if none matches, boot is refused rather than running blind.

| Version | APK / OBB | Status |
| --- | --- | --- |
| **1.6.10** (2013) | `main.7.com.ea.game.pvz2_na.obb` | 🟢 Boots to the menu |
| **4.5.2** (2016) | `main.147.com.ea.game.pvz2_row.obb` | 🟢 Boots to the menu |

Adding a new version = one entry in `kVersions` in [symbols.cpp](pvz2native/src/game/symbols.cpp). Nothing in `runtime/` or `engine/` changes between versions.

---

## ⚙️ Requirements

- **64-bit Windows** (the executable is static x86_64; there is no 32-bit build — dynarmic only ships backends for x86_64/arm64/riscv64).
- **MinGW-w64** (GCC with C++20 support) and **CMake**.
- A **GPU with OpenGL 2.0** or higher (GLSL 1.10 / `#version 110`).
- Your own copy of **`libPVZ2.so`** and the matching **`.obb`**, extracted from the Android game.

---

## 🔨 Building

The project builds with MinGW + CMake. The quick way is [compile.bat](compile.bat):

```bat
compile.bat
```

> [!NOTE]
> `compile.bat` has toolchain paths **hardcoded to the author's machine** (`CMAKE_C_COMPILER`, `CMAKE_MAKE_PROGRAM`, `BOOST_ROOT`, `Python_EXECUTABLE`). Adjust them to your own before using it, or invoke CMake by hand:

```bash
mkdir build && cd build
cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
```

The executable ends up at:

```text
build/pvz2native/pvz2native.exe
```

Incremental rebuild (after the first `cmake`):

```bash
cd build
make pvz2native      # with the MinGW make
```

---

## ▶️ Running

1. Place the game files next to the executable, by default in a `lib/` folder:

   ```text
   build/pvz2native/
   ├── pvz2native.exe
   ├── config.ini            (generated automatically on first launch)
   └── lib/
       ├── libPVZ2.so
       └── main.7.com.ea.game.pvz2_na.obb
   ```

2. Run `pvz2native.exe`. On the first launch it writes a `config.ini` with every option documented and turned off.

If your files live elsewhere or you want to try 4.5.2, edit `config.ini`:

```ini
[paths]
so  = lib/libPVZ2.so
obb = lib/main.7.com.ea.game.pvz2_na.obb
```

---

## 🔧 Configuration (`config.ini`)

Single source of truth for paths and debugging (the old `PVZ2_*` environment variables are no longer read). Excerpt of the most useful sections:

| Section | Key | What it does |
| --- | --- | --- |
| `[paths]` | `so`, `obb` | Paths to the `.so` and `.obb` (relative to the `.exe`, or absolute). |
| `[log]` | `verbose` | One banner per `.init_array` entry and lifecycle call. |
| `[log]` | `trace` | Per-import-call trace. **Very loud** (~57 MB of stdout per run). |
| `[log]` | `pc_sample` | PC sampling / hot-frame slicing. |
| `[gl]` | `strict` | Queries GL state after every suspicious call and reports mismatches. |
| `[gl]` | `debug_clear` | A loud clear colour instead of the engine's opaque black. |
| `[runtime]` | `heap_quarantine` | Holds the N most-recently-freed blocks out of the reuse pool (hunts use-after-free). |
| `[game]` | `user_locale` | The locale the game is told about, e.g. `en_US`. |

---

## ⌨️ Controls

| Action | Control |
| --- | --- |
| Tap / select | Left click |
| Drag (swipe) | Left click + move |
| Type text | Keyboard (only when the game opens a field) |
| Confirm / Delete | `Enter` / `Backspace` |
| **Quit** | `ESC` or close the window |

The window is resizable: the scene renders at a fixed resolution and the compositor scales it, remapping clicks back into the game's space.

---

## 📊 Project status

| Area | Status |
| --- | --- |
| `.so` loading and engine boot | 🟢 |
| Lifecycle (init / surface / frames) | 🟢 |
| Resource loading (RSB/RSG from `.obb`) | 🟢 |
| Native imports (libc/libm/GLES/…) | 🟢 All have a handler |
| Rendering | 🟡 In progress |
| Audio (OpenSL ES → SDL) | 🟡 In progress |
| Input (touch/mouse/keyboard) | 🟡 In progress |
| Saving and online services | 🔴 Not implemented (deliberately offline) |

---

## 📁 Project structure

```text
PvZ2Native/
├── pvz2native/            ← this project's code
│   ├── src/
│   │   ├── runtime/       ← JIT (dynarmic), GuestHeap, threads, synchronization, RSB
│   │   ├── dependencies/  ← one module per Android .so (libc, libm, GLES, …) + VFS
│   │   ├── dex/           ← fake JNIEnv/JavaVM + per-Java-class hooks
│   │   ├── engine/        ← boot / lifecycle / frame
│   │   ├── game/          ← symbols.cpp: addresses per binary version
│   │   ├── diagnostics/   ← watchpoints, probes, PC sampling
│   │   ├── gfx/, audio/, input/, patch/, elf32/
│   │   ├── config.cpp     ← config.ini reader
│   │   └── main.c         ← SDL window + frame loop
│   └── include/pvz2native/
├── dynarmic/              ← ARM JIT (submodule/dependency)
├── SDL/  glad/  zlib/  stb/  third_party/
├── compile.bat           ← MinGW build
├── CMakeLists.txt
└── README.md
```

---

## 📜 License

Code under the **MIT license** (see [LICENSE](LICENSE)). The project reuses third-party emulation infrastructure (dynarmic, SDL, zlib, glad, stb), each with its own license.

**Plants vs. Zombies 2**, its trademarks and all its assets belong to **Electronic Arts / PopCap Games**. This project **does not distribute or replace** any game file: you must supply your own, obtained legally.

## ⚠️ Disclaimer

Independent project, **not affiliated with, endorsed by, or sponsored by** Electronic Arts, PopCap Games or any entity related to Plants vs. Zombies. Its purpose is strictly **technical and educational**: research into ARM binary emulation, compatibility layers, and API translation.

---

<p align="center">
  <b>PvZ2Native</b> — Running Android's <code>libPVZ2.so</code> on PC through ARM emulation
</p>

Sprout
=====

Plants vs. Zombies 2 — Native Windows Port


Runs the Android version of PvZ2 natively on Windows x64, using
a Dynarmic-based ARM64 emulator and OpenGL translation layer.


═══════════════════════════════════════════════════════════
  REQUIREMENTS
═══════════════════════════════════════════════════════════

• Windows 7 or later (x64)
• GPU with OpenGL 2.0+ support (any integrated graphics works)
• Microsoft Visual C++ Redistributable 2015–2022 (x64)
  — included as MSVCP140.dll / VCRUNTIME140.dll


═══════════════════════════════════════════════════════════
  INSTALLATION
═══════════════════════════════════════════════════════════

1. Extract the .zip to any folder
2. Run sprout.exe

The launcher lets you configure:
  • Window mode        – Auto (aspect) / Native (full)
  • FPS limit          – Uncapped / 30 / 60 / 120 / 144
  • VSync              – On / Off
  • Quality            – Low / Medium / High
  • Shadows            – Low / Medium / High
  • Render scale       – 50% / 75% / 100% / 125% / 150%
  • Language           – 12 supported locales
  • Emulate IAP        – Free shop (no real purchases)
  • Persist saves      – Keep save data between launches
  • Show console       – Debug / stdout window


═══════════════════════════════════════════════════════════
  CONTROLS
═══════════════════════════════════════════════════════════

Click the gamepad icon (🎮) in the launcher to open the
controls editor. Every action is fully remappable.


  Keyboard default bindings:
  ───────────────────────────
  1–7           Plant slots 1–7
  J / K / L     Powers 1–3
  Q             Shovel
  G             Plant food (nutrients)
  H             Fast-forward (×2 speed)
  Enter         Pause / Back  (press twice fast to quit)
  F3            Toggle FPS counter
  F4            Toggle coordinate overlay
  F11           Toggle fullscreen

  Gamepad (Xbox / PlayStation):
  ─────────────────────────────────
  Left stick    Touch cursor movement
  A             Tap (primary action)
  B             Back key
  Start         Menu / pause key
  X / Y / LB / RB / Back / Guide /
  L-Stick / R-Stick / D-Pad          Remappable to any action


  Gamepad action defaults:
  ─────────────────────────
  X             Plant slot 1
  Y             Plant slot 2
  LB            Plant slot 3
  RB            Plant slot 4
  Back          Plant slot 5
  L-Stick       Power 1
  R-Stick       Power 2
  D-Up          Power 3
  D-Down        Shovel
  D-Right       Fast-forward
  Start / Guide Pause
  D-Left        (unbound by default)
  Nutrients     (unbound by default)


═══════════════════════════════════════════════════════════
  CONFIGURATION FILE
═══════════════════════════════════════════════════════════

config.ini is auto-generated. You can override every binding
by editing the [controls] section:

    [controls]
    slot_1      = 49      ; SDL keycode for keyboard
    gp_slot_1   = 2       ; SDL_GameControllerButton (0=A, 1=B … 14=D-Right)

Full gamepad button index reference:

    0  = A          5  = Guide        10 = Right Bumper
    1  = B          6  = Start        11 = D-Pad Up
    2  = X          7  = Left Stick   12 = D-Pad Down
    3  = Y          8  = Right Stick  13 = D-Pad Left
    4  = Back       9  = Left Bumper  14 = D-Pad Right

Set any gp_ value to -1 to unbind that action from gamepad.


═══════════════════════════════════════════════════════════
  FILES
═══════════════════════════════════════════════════════════

sprout.exe        — Settings launcher (run this first)
pvz2native.exe    — Game executable
config.ini        — Configuration file (auto-generated)
lib/libPVZ2.so    — Game engine library (~18 MB)
lib/*.obb         — Game assets (~650 MB, required)
userdata/         — Save files (auto-created)


═══════════════════════════════════════════════════════════
  NOTES
═══════════════════════════════════════════════════════════

• First launch may take a moment while the engine
  initialises the emulator and decompresses assets.
• All save data lives in the userdata/ folder —
  delete it to start over.
• No internet connection required.
• Double-tap Enter (within 800 ms) to quit the game.


═══════════════════════════════════════════════════════════
  BUILDING FROM SOURCE
═══════════════════════════════════════════════════════════

Prerequisites: CMake 3.1+, MSVC 2019+ or MinGW-w64 (x64).

  git clone --recursive https://github.com/…
  cd PvZ2Native

  # Game (MinGW) — see compile.bat for full toolchain paths
  cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
  cmake --build build

  # Launcher (MSVC)
  cmake -B build-x64 -G "Visual Studio 17 2022" -A x64
  cmake --build build-x64 --config Release --target launcher

  # Tests
  cmake --build build-x64 --config Release --target test_actions
  build-x64/Release/test_actions.exe


═══════════════════════════════════════════════════════════
  CREDITS
═══════════════════════════════════════════════════════════

• Maximo       – Creator of Sprout
• Optijuegos   – Founder, original concept & project lead
• Dynarmic     – ARM64 emulation
• SDL2         – Windowing & input
• GLAD / OpenGL – Graphics translation
• zlib         – Compression
• PopCap / EA  – Original game assets

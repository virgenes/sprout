# PvZ2Native

📖 **Español** · [English](README.md)

**Ejecuta la librería nativa real de *Plants vs. Zombies 2* para Android (`libPVZ2.so`, ARM32) en PC.** Lo único que se emula es el **procesador**: PvZ2 solo existe compilado para ARM (no hay build x86/x64), así que un JIT traduce sus instrucciones ARM a x86_64. Todo lo demás —Android, JNI, libc, OpenGL ES, sistema de archivos— **no se emula, se reimplementa de forma nativa**, igual que hace **Wine** con Windows.

> [!WARNING]
> Proyecto experimental y en desarrollo activo. Arranca hasta la carga del menú principal; el renderizado, el audio y la entrada están en progreso. **No incluye el juego**: debes aportar tu propio `libPVZ2.so` y tu propio `.obb`.

---

## 📖 Qué es (y qué NO es)

No es un *port* del código fuente del juego. Es un **híbrido de dos técnicas**, y la distinción importa:

- **Solo la CPU se emula.** El binario del juego es código máquina ARM y PvZ2 nunca se compiló para x86/x64, así que no hay forma de ejecutarlo directamente en un procesador de PC. Un JIT (dynarmic) lee esas instrucciones ARM y las traduce a x86_64 sobre la marcha. Esta es la única parte "emulada".
- **Android NO se emula: se reimplementa.** Cuando el juego llama a una función de Android (abrir un archivo, dibujar con OpenGL ES, reservar memoria, invocar Java…), esa llamada la atiende **código nativo de PC** que hace el trabajo real usando la API equivalente del escritorio. No hay un Android virtual corriendo debajo; hay una **reimplementación** de las funciones que el juego necesita.

Es exactamente el enfoque de **Wine** — *"Wine Is Not an Emulator"* — llevado un paso más allá: Wine reimplementa las llamadas de Windows sin emular nada porque los `.exe` ya son x86; aquí, como el juego es ARM, además hay que emular la CPU. El resto de la filosofía es idéntica: **traducir llamadas, no simular una máquina completa.**

```text
┌──────────────────────────────────────────────┐
│   libPVZ2.so  (ELF ARM32, EABI5, sin tocar)   │   ← el juego real de Android
└───────────────────────┬──────────────────────┘
                        │  instrucciones ARM
                        ▼
┌──────────────────────────────────────────────┐
│   dynarmic  —  JIT ARM32 → x86_64             │   ← ÚNICO componente emulado
└───────────────────────┬──────────────────────┘      (solo la CPU)
                        │  llamadas a "Android"
                        ▼
┌──────────────────────────────────────────────┐
│   Capa de compatibilidad (este proyecto)      │   ← reimplementación NATIVA,
│                                               │      estilo Wine (nada emulado)
│   JNI / JavaVM falso   ·   libc / libm / libz │
│   OpenGL ES → OpenGL   ·   OpenSL ES → SDL    │
│   GuestHeap propio     ·   VFS (rutas + .obb) │
└───────────────────────┬──────────────────────┘
                        │
                        ▼
┌──────────────────────────────────────────────┐
│   Windows x64  ·  SDL2  ·  OpenGL 2.0 (glad)  │
└──────────────────────────────────────────────┘
```

Cuando el juego pide algo del sistema operativo (abrir un archivo, reservar memoria, dibujar, reproducir sonido, llamar a Java), esa llamada sale del código emulado como una interrupción y la atiende un *handler* de esta capa, respaldado por la API equivalente de PC.

---

## 🧩 Arquitectura

El código está en [pvz2native/](pvz2native/) y se divide en capas con una regla estricta: **solo una capa puede contener direcciones concretas del `.so`.** Eso es lo que hace que soportar otra versión del juego sea sumar datos, no reescribir lógica.

| Capa | Pregunta que responde | ¿Puede tener una dirección del `.so`? |
| --- | --- | :---: |
| [runtime/](pvz2native/src/runtime/) | Cómo ejecutar código ARM (JIT, heap, hilos, sincronización) | **Nunca** |
| [game/symbols.cpp](pvz2native/src/game/symbols.cpp) | Dónde está cada cosa en *este* binario | **La única que sí** |
| [engine/](pvz2native/src/engine/) | Qué llamar y en qué orden (boot, lifecycle, frame) | Solo vía `sym()` |
| [dependencies/](pvz2native/src/dependencies/) | Cómo responder a cada `.so` de Android (libc, libm, GLES, …) | No |
| [dex/](pvz2native/src/dex/) | Cómo responder al lado Java (JNIEnv falso + hooks por clase) | No |
| [diagnostics/](pvz2native/src/diagnostics/) | Cómo interrogar al invitado (watchpoints, muestreo de PC) | Solo vía `sym()` |

Piezas destacadas:

- **JIT:** [dynarmic](dynarmic/) (solo frontend A32). Un JIT por hilo invitado, con *page table* de identidad sobre la memoria del `.so` para velocidad.
- **Dependencias nativas:** un módulo por `.so` de Android que el juego enlaza. La libc está partida por cabecera (`string`, `stdio`, `stdlib`, `time`, `pthread`, `locale`, `ctype`, …), más `libm`, `libz`, `liblog`, `libgles`, `libstdcxx`, `libopensles`, `libdl`. Cada handler es un `void(GuestCall&)` registrado por nombre.
- **Lado Java:** [jni_env.cpp](pvz2native/src/dex/jni_env.cpp) implementa un `JNIEnv`/`JavaVM` falso (objetos, strings, arrays, referencias); [dex/hooks/](pvz2native/src/dex/hooks/) tiene un archivo por clase Java (`AndroidGameApp`, `AndroidSurfaceView`, HTTP, Google Play, Facebook…).
- **Gráficos:** las llamadas GLES del juego se traducen a **OpenGL 2.0 (perfil de compatibilidad)** vía `glad`; SDL provee la ventana y el contexto GL, haciendo el papel del `GLSurfaceView` de Android.
- **Recursos:** un VFS traduce rutas de Android a rutas de PC y lee los assets desde el `.obb` real del juego (formato RSB/RSG).

---

## 🎮 Versiones del juego soportadas

Se detecta la versión por **huella de bytes** en dos funciones nativas conocidas; si no coincide ninguna, el arranque se rechaza en vez de correr a ciegas.

| Versión | APK / OBB | Estado |
| --- | --- | --- |
| **1.6.10** (2013) | `main.7.com.ea.game.pvz2_na.obb` | 🟢 Arranca al menú |
| **4.5.2** (2016) | `main.147.com.ea.game.pvz2_row.obb` | 🟢 Arranca al menú |

Añadir una versión nueva = una entrada en `kVersions` de [symbols.cpp](pvz2native/src/game/symbols.cpp). Nada de `runtime/` ni `engine/` cambia entre versiones.

---

## ⚙️ Requisitos

- **Windows de 64 bits** (el ejecutable es x86_64 estático; no hay build de 32 bits — dynarmic solo trae backends para x86_64/arm64/riscv64).
- **MinGW-w64** (GCC con soporte C++20) y **CMake**.
- **GPU con OpenGL 2.0** o superior (GLSL 1.10 / `#version 110`).
- Tu propia copia de **`libPVZ2.so`** y del **`.obb`** correspondiente, extraídos del juego de Android.

---

## 🔨 Compilación

El proyecto se compila con MinGW + CMake. La forma rápida es [compile.bat](compile.bat):

```bat
compile.bat
```

> [!NOTE]
> `compile.bat` tiene rutas de toolchain **fijadas a la máquina del autor** (`CMAKE_C_COMPILER`, `CMAKE_MAKE_PROGRAM`, `BOOST_ROOT`, `Python_EXECUTABLE`). Ajústalas a las de tu equipo antes de usarlo, o invoca CMake a mano:

```bash
mkdir build && cd build
cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
```

El ejecutable queda en:

```text
build/pvz2native/pvz2native.exe
```

Recompilación incremental (tras el primer `cmake`):

```bash
cd build
make pvz2native      # con el make de MinGW
```

---

## ▶️ Ejecución

1. Coloca los archivos del juego junto al ejecutable, por defecto en una carpeta `lib/`:

   ```text
   build/pvz2native/
   ├── pvz2native.exe
   ├── config.ini            (se genera solo en el primer arranque)
   └── lib/
       ├── libPVZ2.so
       └── main.7.com.ea.game.pvz2_na.obb
   ```

2. Ejecuta `pvz2native.exe`. En el primer arranque escribe un `config.ini` con todas las opciones documentadas y desactivadas.

Si tus archivos están en otro sitio o quieres probar la 4.5.2, edita `config.ini`:

```ini
[paths]
so  = lib/libPVZ2.so
obb = lib/main.7.com.ea.game.pvz2_na.obb
```

---

## 🔧 Configuración (`config.ini`)

Fuente única de verdad para rutas y depuración (las antiguas variables `PVZ2_*` ya no se leen). Extracto de las secciones más útiles:

| Sección | Clave | Para qué sirve |
| --- | --- | --- |
| `[paths]` | `so`, `obb` | Rutas al `.so` y al `.obb` (relativas al `.exe` o absolutas). |
| `[log]` | `verbose` | Un banner por cada entrada de `.init_array` y llamada de ciclo de vida. |
| `[log]` | `trace` | Traza por llamada a import. **Muy ruidoso** (~57 MB de stdout por run). |
| `[log]` | `pc_sample` | Muestreo de PC / troceo de frames calientes. |
| `[gl]` | `strict` | Consulta el estado de GL tras cada llamada sospechosa y reporta discrepancias. |
| `[gl]` | `debug_clear` | Color de fondo llamativo en vez del negro opaco del motor. |
| `[runtime]` | `heap_quarantine` | Retiene los N bloques recién liberados fuera del pool (caza use-after-free). |
| `[game]` | `user_locale` | Locale que se le dice al juego, como `en_US`. |

---

## ⌨️ Controles

| Acción | Control |
| --- | --- |
| Tocar / seleccionar | Clic izquierdo |
| Arrastrar (deslizar) | Clic izquierdo + mover |
| Escribir texto | Teclado (solo cuando el juego abre un campo) |
| Confirmar / Borrar | `Enter` / `Backspace` |
| **Salir** | `ESC` o cerrar la ventana |

La ventana es redimensionable: la escena se renderiza a resolución fija y el compositor la escala, remapeando los clics al espacio del juego.

---

## 📊 Estado del proyecto

| Área | Estado |
| --- | --- |
| Carga del `.so` y arranque del motor | 🟢 |
| Ciclo de vida (init / surface / frames) | 🟢 |
| Carga de recursos (RSB/RSG desde `.obb`) | 🟢 |
| Imports nativos (libc/libm/GLES/…) | 🟢 Todos con handler |
| Renderizado | 🟡 En progreso |
| Audio (OpenSL ES → SDL) | 🟡 En progreso |
| Entrada (touch/mouse/teclado) | 🟡 En progreso |
| Guardado y servicios online | 🔴 No implementado (deliberadamente offline) |

---

## 📁 Estructura del proyecto

```text
PvZ2Native/
├── pvz2native/            ← código de este proyecto
│   ├── src/
│   │   ├── runtime/       ← JIT (dynarmic), GuestHeap, hilos, sincronización, RSB
│   │   ├── dependencies/  ← un módulo por .so de Android (libc, libm, GLES, …) + VFS
│   │   ├── dex/           ← JNIEnv/JavaVM falso + hooks por clase Java
│   │   ├── engine/        ← boot / lifecycle / frame
│   │   ├── game/          ← symbols.cpp: direcciones por versión del binario
│   │   ├── diagnostics/   ← watchpoints, sondas, muestreo de PC
│   │   ├── gfx/, audio/, input/, patch/, elf32/
│   │   ├── config.cpp     ← lector de config.ini
│   │   └── main.c         ← ventana SDL + bucle de frames
│   └── include/pvz2native/
├── dynarmic/              ← JIT ARM (submódulo/dependencia)
├── SDL/  glad/  zlib/  stb/  third_party/
├── compile.bat           ← build con MinGW
├── CMakeLists.txt
└── README.md
```

---

## 📜 Licencia

Código bajo **licencia MIT** (ver [LICENSE](LICENSE)). El proyecto reutiliza infraestructura de emulación de terceros (dynarmic, SDL, zlib, glad, stb), cada uno con su propia licencia.

**Plants vs. Zombies 2**, sus marcas y todos sus recursos pertenecen a **Electronic Arts / PopCap Games**. Este proyecto **no distribuye ni reemplaza** ningún archivo del juego: debes aportar los tuyos, obtenidos legalmente.

## ⚠️ Descargo de responsabilidad

Proyecto independiente, **sin afiliación, respaldo ni patrocinio** de Electronic Arts, PopCap Games ni ninguna entidad relacionada con Plants vs. Zombies. Su fin es exclusivamente **técnico y educativo**: investigación de emulación de binarios ARM, capas de compatibilidad y traducción de APIs.

---

<p align="center">
  <b>PvZ2Native</b> — Ejecutando la <code>libPVZ2.so</code> de Android en PC mediante emulación ARM
</p>

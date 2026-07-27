# Sprout 🌱

**Español** · [English](README.md)

**Sprout** es una solución completa de escritorio para ejecutar *Plants vs. Zombies 2* en PC. Combina una capa de emulación ARM (el motor) con un lanzador gráfico con todas las funciones — editor de controles, soporte para gamepad, renderizado OpenGL 2.0 y mucho más — todo en un solo paquete.

> **Debes aportar tus propios archivos del juego.** Sprout no distribuye `libPVZ2.so`, archivos `.obb` ni ningún otro material protegido por derechos de autor.

---

## Índice

- [Características](#características)
- [Arquitectura](#arquitectura)
- [Lanzador](#lanzador)
- [Controles](#controles)
- [Versiones soportadas](#versiones-soportadas)
- [Inicio rápido](#inicio-rápido)
- [Compilación](#compilación)
- [Estructura del proyecto](#estructura-del-proyecto)
- [Créditos](#créditos)
- [Licencia](#licencia)

---

## Características

### Lanzador

- **Editor visual de controles** — reasigna cada acción del juego a cualquier tecla o botón del gamepad desde una interfaz gráfica limpia
- **Soporte para gamepad** — mandos Xbox, PlayStation y genéricos; detección de entrada en vivo; mapeo por acción con zona muerta y configuración de ejes
- **Selector de idioma** — define el locale interno del juego (`en_US`, `es_ES`, etc.)
- **IAP emuladas** — activa respuestas simuladas de compras para pruebas
- **Configuración persistente** — todos los ajustes guardados en `config.ini`, generado automáticamente al primer inicio
- **Ruta del juego** — selecciona tu `libPVZ2.so` y `.obb` desde la interfaz

### Motor

- **Emulación JIT de la CPU** — [dynarmic](https://github.com/dynarmic/dynarmic) traduce instrucciones ARM32 a x86_64 en tiempo real
- **Reimplementación de Android** (estilo Wine) — OpenGL ES → OpenGL 2.0, OpenSL ES → audio SDL, JNI/JavaVM falso para llamadas Java
- **Gestor de memoria invitada** — heap seguro con zona de cuarentena para detectar use-after-free
- **Sistema de archivos virtual** — mapea rutas de Android a ubicaciones de PC, lee assets del `.obb` (formato RSB/RSG)
- **Soporte multiversión** — huellas digitales de bytes detectan la versión del juego y cargan los offsets correctos automáticamente

### Gráficos y entrada

- **OpenGL 2.0 perfil de compatibilidad** — funciona en prácticamente cualquier GPU
- **Ventana redimensionable** con escalado automático y remapeo de clics
- **Limitador de fotogramas** para velocidad de juego consistente
- **Mapeo completo de teclado + ratón** — cada acción del juego configurable
- **Conexión en caliente del gamepad** — conecta y desconecta sin reiniciar

---

## Arquitectura

El código original era un **monolito** — las direcciones del juego, la lógica del runtime y los manejadores de llamadas del sistema estaban entremezclados. Soportar una versión nueva implicaba buscar offsets hardcodeados por todo el código.

Sprout lo refactorizó como una **arquitectura de orquestador** con una regla estricta de capas:

> **Solo una capa puede contener direcciones concretas del `.so`.**
> El resto opera exclusivamente a través de esa capa.

### Diagrama de capas

```
┌──────────────────────────────────────────────────────────┐
│                    LANZADOR (GUI)                         │
│  editor de controles · gamepad · IAP · locale             │
└────────────────────────┬─────────────────────────────────┘
                         │ lanza
                         ▼
┌──────────────────────────────────────────────────────────┐
│  MOTOR (orquestador)                                      │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐               │
│  │ engine/  │  │  game/   │  │  dex/    │               │
│  │ boot     │  │ symbols  │  │  JNIEnv  │               │
│  │ frame    │──│ (direcc.)│  │  hooks   │               │
│  │ lifecycle│  └──────────┘  └──────────┘               │
│  └──────────┘                                            │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐               │
│  │runtime/  │  │dependenc.│  │diagnostic│               │
│  │JIT, heap │  │libc, GLES│  │sondas    │               │
│  └──────────┘  └──────────┘  └──────────┘               │
└──────────────────────────────────────────────────────────┘
```

### Responsabilidad de cada capa

| Capa | Responsabilidad | ¿Tiene direcciones del `.so`? |
|---|---|---|
| [runtime/](pvz2native/src/runtime/) | Ejecución JIT, heap invitado, hilos, sincronización, caché RSB | **Nunca** |
| [game/symbols.cpp](pvz2native/src/game/symbols.cpp) | Tabla de símbolos — mapea nombres de función a direcciones por versión | **La única** |
| [engine/](pvz2native/src/engine/) | Secuencia de arranque, ciclo de vida, bucle de frames — llama a símbolos mediante `sym()` | Solo vía `sym()` |
| [dependencies/](pvz2native/src/dependencies/) | Manejadores para cada `.so` de Android (libc, libm, GLES, OpenSL ES, libz, libdl, libstdcxx, liblog) | No |
| [dex/](pvz2native/src/dex/) | `JNIEnv`/`JavaVM` falso + un archivo por clase Java hookeada | No |
| [diagnostics/](pvz2native/src/diagnostics/) | Watchpoints, muestreo de PC, sondas del invitado | Solo vía `sym()` |

### Por qué importa

Añadir soporte para una versión nueva del juego requiere **exactamente un cambio**: una entrada nueva en `kVersions` dentro de `symbols.cpp`. El motor, runtime, dependencias y hooks quedan intactos.

El motor orquesta el arranque sin saber dónde vive nada en memoria. Las dependencias se registran por nombre y se despachan automáticamente. El lanzador maneja todo a través de archivos de configuración — no hace falta recompilar para cambiar de versión.

---

## Lanzador

El lanzador (`sprout.exe`) provee una interfaz gráfica completa para configurar y ejecutar el juego:

- **Pestaña Controles** — cuadrícula visual con cada acción del juego mapeada a tu teclado/gamepad. Haz clic en cualquier entrada para reasignarla. Alterna entre diseños de teclado y gamepad.
- **Pestaña Gamepad** — configura zona muerta de sticks, sensibilidad de gatillos, invertir ejes y prueba la entrada en vivo
- **Pestaña Gráficos** — modo ventana, resolución, límite de FPS
- **Pestaña Idioma** — elige el idioma de visualización del juego desde un desplegable
- **Toggle IAP** — activa/desactiva compras simuladas con un interruptor
- **Pestaña Rutas** — busca tus archivos `libPVZ2.so` y `.obb`

Todos los ajustes se guardan en `config.ini`, completamente documentado con comentarios inline.

---

## Controles

### Diseño de teclado por defecto

| Acción | Tecla |
|---|---|
| Tocar / seleccionar | Clic izquierdo |
| Arrastrar / deslizar | Clic izquierdo + mover |
| Escribir texto | Teclado (cuando hay un campo de texto) |
| Confirmar | `Enter` |
| Borrar / Volver | `Backspace` |
| Pausa | `Escape` |
| Colocar planta (slot 1–8) | `1`–`8` |
| Acelerar | `Space` |

### Gamepad

- Todas las acciones se pueden mapear desde el editor de controles del lanzador
- Stick izquierdo → arrastrar, Stick derecho → desplazar (configurable)
- Botones frontales → slots de planta, gatillos → confirmar/cancelar
- Start → pausa, Select → acelerar

Cada combinación es personalizable. El lanzador detecta tu modelo de control y aplica valores por defecto sensatos.

---

## Versiones soportadas

La versión del juego se detecta mediante **huella de bytes** en dos funciones nativas conocidas. Si ninguna coincide, se rechaza el arranque en vez de ejecutar a ciegas.

| Versión | OBB | Estado |
|---|---|---|
| **1.6.10** (2013) | `main.7.com.ea.game.pvz2_na.obb` | Arranca al menú |
| **4.5.2** (2016) | `main.147.com.ea.game.pvz2_row.obb` | Arranca al menú |

Para añadir una versión: agrega una entrada a `kVersions` en [symbols.cpp](pvz2native/src/game/symbols.cpp).

---

## Inicio rápido

1. Descarga la última versión desde la página de [Releases](https://github.com/virgenes/sprout/releases)
2. Extrae `Sprout_v1.0.zip` en una carpeta
3. Copia tu `libPVZ2.so` y el `.obb` correspondiente a la subcarpeta `lib/`
4. Ejecuta `Sprout.exe`
5. Se abre el lanzador — configura controles y rutas, luego haz clic en **Launch**

### Configuración manual

Crea esta estructura junto a `sprout.exe`:

```
├── Sprout.exe
├── config.ini          (se genera solo al primer inicio)
└── lib/
    ├── libPVZ2.so
    └── main.7.com.ea.game.pvz2_na.obb
```

---

## Compilación

### Requisitos

- **Windows x64**
- **MinGW-w64** (GCC con soporte C++20)
- **CMake** ≥ 3.16
- **Boost** (para dynarmic)
- **Python 3** (para scripts de build)
- GPU con **OpenGL 2.0** o superior

### Compilar

```bat
compile.bat
```

> El `.bat` tiene rutas de toolchain fijadas a la máquina del autor. Ajusta `CMAKE_C_COMPILER`, `CMAKE_MAKE_PROGRAM`, `BOOST_ROOT` y `Python_EXECUTABLE` a tu configuración, o invoca CMake manualmente:

```bash
mkdir build && cd build
cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
```

Los binarios quedan en `build/sprout/`.

---

## Estructura del proyecto

```
Sprout/
├── pvz2native/               ← fuente del motor + lanzador
│   ├── include/sprout/       ← headers públicos
│   ├── src/                  ← implementación del motor
│   │   ├── runtime/          ← JIT, heap invitado, hilos
│   │   ├── dependencies/     ← reimplementaciones de .so de Android
│   │   ├── dex/              ← JNI/JavaVM falso + hooks de clases
│   │   ├── engine/           ← boot, ciclo de vida, bucle de frames
│   │   ├── game/             ← símbolos (la única capa con direcciones)
│   │   ├── diagnostics/      ← watchpoints, sondas
│   │   ├── gfx/              ← traducción OpenGL ES → OpenGL
│   │   ├── audio/            ← OpenSL ES → audio SDL
│   │   ├── input/            ← teclado, ratón, gamepad
│   │   ├── patch/            ← parcheo de código en runtime
│   │   └── elf32/            ← cargador ELF
│   ├── launcher/             ← GUI (editor de controles, config, etc.)
│   └── tests/                ← pruebas unitarias
├── CMakeLists.txt
├── compile.bat
└── README.md
```

---

## Créditos

- **[Maximo](https://www.pvz2.app/)** — idea original e investigación fundamental que hizo posible este proyecto
- **[Optijuegos](https://www.youtube.com/@Optijuegos)** — apoyo de la comunidad y pruebas beta

---

## Licencia

El código tiene **licencia MIT** (ver [LICENSE](LICENSE)). Las dependencias de terceros (dynarmic, SDL, zlib, glad, stb) tienen sus propias licencias.

*Plants vs. Zombies 2* y todos los recursos relacionados son marcas registradas de **Electronic Arts / PopCap Games**. Este proyecto **no está afiliado, respaldado ni patrocinado por** EA o PopCap. No distribuye ningún archivo del juego — debes aportar los tuyos, obtenidos legalmente.

---

<p align="center">
  <b>Sprout</b> — Cultivando PvZ2 en PC, una llamada nativa a la vez
</p>

# Sprout 🌱

**Español** · [English](README.md)

**Sprout** es una solución completa de escritorio para ejecutar *Plants vs. Zombies 2* en PC. Combina una capa de emulación ARM (el motor) con un lanzador gráfico con todas las funciones — editor de controles, soporte para gamepad, renderizado OpenGL 2.0, renderizado por software (Mesa3D), diagnóstico del sistema y mucho más — todo en un solo paquete.

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

### Lanzador (Rediseño Moderno V2)

- **Escáner de Diagnóstico del Sistema** — escaneo automático en segundo plano al iniciar que verifica capacidades OpenGL de la GPU, archivos del renderizador por software Mesa3D, VC++ Runtimes, memoria RAM, espacio en disco y extensiones de CPU SIMD (AVX2/SSE4.1)
- **Descargas Automatizadas e Instaladores (WinHTTP)** — gestor de descargas integrado de un solo clic con soporte para redirecciones múltiples para obtener e instalar dependencias faltantes (motor Mesa3D, VC++ Redistributable) automáticamente
- **Localizar / Importar Mesa3D (Botón de Carpeta 📁)** — diálogo nativo de archivos de Windows para ubicar e importar binarios `opengl32.dll` existentes, carpetas Mesa3D o paquetes `.7z`/`.zip` directamente
- **Optimización Extrema para PCs de Bajas Prestaciones** — construido con pintado GDI personalizado super ligero y consumo mínimo de recursos, diseñado para funcionar fluido en PCs antiguas sin parpadeos de ventana (`WS_CLIPCHILDREN`)
- **Editor visual de controles** — reasigna cada acción del juego a cualquier tecla o botón del gamepad desde una interfaz gráfica limpia
- **Interfaz de Navegación con 4 Pestañas** — navegación fluida entre **Inicio**, **Diagnóstico del Sistema**, **Opciones del Juego** y **Ajustes del Motor**
- **Selector de Renderizador (Mesa3D)** — alterna entre renderizado por Hardware (OpenGL por GPU) y Software (CPU vía Mesa3D LLVMpipe) para equipos sin gráficos dedicados
- **Control de Anti-Aliasing MSAA** — ajusta desactivado, 2x, 4x o 8x Multisample Anti-Aliasing directamente desde la pestaña del motor
- **Soporte para gamepad** — mandos Xbox, PlayStation y genéricos; detección de entrada en vivo y mapeo de acciones
- **Selector de Idioma y Funciones** — elige el locale (`en_US`, `es_ES`, etc.), alterna IAPs emuladas, V-Sync, guardado persistente y consola de depuración

### Motor

- **Emulación JIT de la CPU** — [dynarmic](https://github.com/dynarmic/dynarmic) traduce instrucciones ARM32 a x86_64 en tiempo real
- **Reimplementación de Android** (estilo Wine) — OpenGL ES → OpenGL 2.0, OpenSL ES → audio SDL, JNI/JavaVM falso para llamadas Java
- **Escáner Automático de Firmas** — motor de coincidencia de patrones (`game_symbols_pattern_scan`) que auto-descubre offsets de funciones en binarios `libPVZ2.so` no listados
- **Framework de Hooks Nativo del Guest** — API de hooks segura (`hook_framework`) para registrar callbacks C++ en direcciones de funciones ARM del juego para modding
- **Manejador de Excepciones Vectoreadas (VEH)** — captura de fallos de página de bajo costo para optimización de fastmem en Windows (`veh_handler`)
- **Gestor de memoria invitada** — heap seguro con zona de cuarentena para detectar use-after-free
- **Sistema de archivos virtual** — mapea rutas de Android a ubicaciones de PC, lee assets del `.obb` (formato RSB/RSG)
- **Soporte multiversión** — huellas digitales de bytes y escáner de firmas detectan versiones del juego automáticamente

### Gráficos y entrada

- **Renderizadores Hardware y Software** — Aceleración gráfica OpenGL 2.0 por GPU o renderizado por software CPU vía Mesa3D
- **Anti-Aliasing MSAA** — pistas de antialiasing multimensurable configurables (`gl_msaa`) en `config.ini`
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
| [src/runtime/](src/runtime/) | Ejecución JIT, heap invitado, hilos, sincronización, manejador VEH, caché RSB | **Nunca** |
| [src/game/symbols.cpp](src/game/symbols.cpp) | Tabla de símbolos — mapea nombres de función a direcciones por versión | **La única** |
| [src/engine/](src/engine/) | Secuencia de arranque, ciclo de vida, bucle de frames — llama a símbolos mediante `sym()` | Solo vía `sym()` |
| [src/dependencies/](src/dependencies/) | Manejadores para cada `.so` de Android (libc, libm, GLES, OpenSL ES, libz, libdl, libstdcxx, liblog) | No |
| [src/dex/](src/dex/) | `JNIEnv`/`JavaVM` falso + un archivo por clase Java hookeada | No |
| [src/diagnostics/](src/diagnostics/) | Watchpoints, muestreo de PC, sondas del invitado | Solo vía `sym()` |

### Por qué importa

Añadir soporte para una versión nueva del juego requiere **exactamente un cambio**: una entrada nueva en `kVersions` dentro de `symbols.cpp`. El motor, runtime, dependencias y hooks quedan intactos.

El motor orquesta el arranque sin saber dónde vive nada en memoria. Las dependencias se registran por nombre y se despachan automáticamente. El lanzador maneja todo a través de archivos de configuración — no hace falta recompilar para cambiar de versión.

---

## Lanzador

El lanzador (`launcher.exe`) provee una interfaz nativa Win32 moderna diseñada para el máximo rendimiento y el menor consumo de memoria en cualquier computadora:

- **Pestaña Inicio** — Insignia de estado del sistema, recomendación automática de configuración (Hardware vs Software) y botón para iniciar el juego.
- **Pestaña Sistema** — Panel de diagnóstico en tiempo real sobre OpenGL, Mesa3D, VC++ Runtime, RAM, Espacio en disco, Windows y compatibilidad AVX2/SSE4.1, junto con botones de descarga automática y **botón de importación 📁** para Mesa3D.
- **Pestaña Opciones** — Modo ventana/pantalla completa, limitador de FPS (30, 60, 120, 144, ilimitado), calidad gráfica, sombras, escala de renderizado, idioma y toggles (V-Sync, IAP emuladas, guardado persistente y consola).
- **Pestaña Motor** — Selector de renderizado (Hardware OpenGL vs Software CPU Mesa3D) y configuración de Anti-Aliasing MSAA.
- **Diálogo de Controles** — Modal completo para reasignación de teclado y gamepad con captura de teclas en vivo.

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

Para añadir una versión: agrega una entrada a `kVersions` en [symbols.cpp](src/game/symbols.cpp).

---

## Inicio rápido

1. Descarga la última versión desde la página de [Releases](https://github.com/virgenes/sprout/releases)
2. Extrae `Sprout_PvZ2_PC.zip` en una carpeta
3. Ejecuta `launcher.exe`
4. El lanzador se abre — verifica los diagnósticos del sistema o haz clic en **Lanzar Juego** (`sprout.exe`)

---

## Compilación

### Requisitos

- **Windows x64**
- **MSVC 2019/2022** o **MinGW-w64** (con soporte C++20)
- **CMake** ≥ 3.16

### Compilar

Usando MSVC (Visual Studio 2019 / 2022):

```bat
compile-msvc.bat
```

O invocando CMake manualmente:

```bash
mkdir build-win32 && cd build-win32
cmake -G "Visual Studio 16 2019" -A x64 ..
cmake --build . --config Release
```

Ejecutables generados:
- `build-win32/Release/launcher.exe` — Lanzador gráfico
- `build-win32/pvz2native/Release/sprout.exe` — Motor del juego

---

## Estructura del proyecto

```
Sprout/
├── include/sprout/          ← Archivos de cabecera C/C++ públicos
│   ├── audio/               ← Cabeceras de OpenSL ES / audio SDL
│   ├── dependencies/        ← Cabeceras de librerías del sistema Android
│   ├── dex/                 ← JNIEnv falso y hooks de clases Java
│   ├── diagnostics/         ← Cabeceras de watchpoints y sondas
│   ├── engine/              ← Cabeceras de arranque y ciclo de vida
│   ├── game/                ← Declaraciones de la tabla de símbolos
│   ├── gfx/                 ← Cabeceras de OpenGL / MSAA / limitador de FPS
│   ├── input/               ← Cabeceras de gamepad y teclado
│   ├── patch/               ← Parcheo de código y desvíos (detours)
│   └── runtime/             ← JIT, heap, VEH, manejador VEH y framework de hooks
├── src/                     ← Implementación principal del motor
│   ├── audio/               ← Salida de audio y repositorio de sonido
│   ├── dependencies/        ← Reimplementaciones de .so de Android (libc, GLES, zlib)
│   ├── dex/                 ← Implementación de Java VM y hooks de clases
│   ├── engine/              ← Secuencia de arranque, bucle de frames, ciclo de vida
│   ├── game/                ← symbols.cpp (la ÚNICA capa con direcciones)
│   ├── gfx/                 ← Capa de traducción de OpenGL
│   ├── input/               ← Hilo de gamepad y cola de entrada
│   ├── patch/               ← Parcheo de código en tiempo de ejecución
│   └── runtime/             ← Wrapper de Dynarmic JIT, heap invitado, VEH, framework de hooks
├── launcher/                ← Lanzador GUI Win32 (main.cpp, scanner.cpp, widgets)
├── mesa/                    ← Binarios y utilidades de renderizado por software Mesa3D
├── tools/                   ← Scripts RE de Ghidra y herramientas de modding RSB/RTON
├── tests/                   ← Pruebas unitarias
├── CMakeLists.txt           ├── Script de compilación principal de CMake
├── compile-msvc.bat         ├── Script de compilación para MSVC 2019/2022
├── readme_spanish.md        ├── Documentación en Español
└── README.md                └── Documentación principal (Inglés)
```

---

## Créditos

- **[Maximo](https://www.pvz2.app/)** — idea original e investigación fundamental que hizo posible este proyecto
- **[Optijuegos](https://www.youtube.com/@Optijuegos)** — apoyo de la comunidad, pruebas y dirección del concepto

---

## Licencia

El código tiene **licencia MIT** (ver [LICENSE](LICENSE)). Las dependencias de terceros (dynarmic, SDL, zlib, glad, stb) tienen sus propias licencias.

*Plants vs. Zombies 2* y todos los recursos relacionados son marcas registradas de **Electronic Arts / PopCap Games**. Este proyecto **no está afiliado, respaldado ni patrocinado por** EA o PopCap.

---

<p align="center">
  <b>Sprout</b> — Cultivando PvZ2 en PC, una llamada nativa a la vez
</p>

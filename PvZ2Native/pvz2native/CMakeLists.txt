cmake_minimum_required (VERSION 3.5)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

project(sprout-core)

file(GLOB_RECURSE SOURCE_FILES CONFIGURE_DEPENDS src/*.c src/*.cpp)

add_executable(sprout ${SOURCE_FILES} ${CMAKE_SOURCE_DIR}/stb/stb_vorbis.c src/sprout.rc)
target_include_directories(sprout PUBLIC include/)
target_link_libraries(sprout ${CMAKE_DL_LIBS} zlibstatic SDL2::SDL2 SDL2main glad dynarmic)
include_directories(sprout PUBLIC include/)
set_target_properties(sprout PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

# Link the MinGW runtime statically so the .exe runs on a machine that has no
# toolchain installed. Everything else here is already static (SDL_STATIC is
# forced on in the root CMakeLists, as are zlib/glad/dynarmic), so the only
# remaining runtime dependencies were the three GCC DLLs -- and copying the
# binary to another PC failed on exactly one of them, libwinpthread-1.dll,
# which std::thread and std::mutex pull in.
#
# 64-bit on purpose, and it has to stay that way: dynarmic ships host backends
# for x86_64, arm64 and riscv64 only. Building with -m32 makes its
# DetectArchitecture report "x86", no backend sources are compiled at all, and
# the link fails on every Dynarmic::A32::Jit symbol. A static x86_64 build runs
# on any 64-bit Windows anyway, which is what -m32 would have been for.
if (MINGW)
    target_link_options(sprout PRIVATE -static)
endif()

# Launcher (native Win32 GUI) — MSVC only, no external dependencies
if(MSVC)
    add_executable(launcher
        launcher/main.cpp
        launcher/config.cpp
        launcher/controls_dialog.cpp
        src/actions.c
        launcher/gamepad_icon.h
        launcher/widgets.h
        launcher/config.h
        launcher/controls_dialog.h
    )
    target_include_directories(launcher PRIVATE include/ launcher/)
    target_link_libraries(launcher PRIVATE user32 gdi32 comctl32 dwmapi shell32 msimg32)
    set_target_properties(launcher PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        WIN32_EXECUTABLE TRUE
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
endif()

# Tests (host executable)
add_executable(test_actions tests/test_actions.c src/actions.c)
target_include_directories(test_actions PRIVATE include/)
set_target_properties(test_actions PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

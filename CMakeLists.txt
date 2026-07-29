cmake_minimum_required(VERSION 3.5)
project(sprout)

if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include_directories(${PROJECT_SOURCE_DIR}/stb)

SET(SDL_STATIC ON CACHE BOOL "Enable SDL_STATIC" FORCE)
SET(SDL_SHARED OFF CACHE BOOL "Disable SDL_SHARED" FORCE)
SET(SDL_MMX OFF CACHE BOOL "Disable SDL_MMX" FORCE)
SET(SDL_SSE OFF CACHE BOOL "Disable SDL_SSE" FORCE)
SET(SDL_SSE2 OFF CACHE BOOL "Disable SDL_SSE2" FORCE)
SET(SDL_SSE3 OFF CACHE BOOL "Disable SDL_SSE3" FORCE)
SET(SDL_SSEMATH OFF CACHE BOOL "Disable SDL_SSEMATH" FORCE)
SET(SDL_3DNOW OFF CACHE BOOL "Disable SDL_3DNOW" FORCE)
SET(SDL_TEST OFF CACHE BOOL "Disable SDL_TEST" FORCE)
SET(SDL_TESTS OFF CACHE BOOL "Disable SDL_TESTS" FORCE)

add_subdirectory(glad/cmake)
glad_add_library(glad REPRODUCIBLE LOADER API egl=1.5 gl:compatibility=2.0)
add_subdirectory(zlib)
add_subdirectory(SDL)

# Dynarmic setup
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "Selects which frontends to enable" FORCE)
set(DYNARMIC_USE_BOOST OFF CACHE BOOL "Disable Boost dependency in dynarmic" FORCE)
set(CMAKE_DISABLE_FIND_PACKAGE_Boost ON CACHE BOOL "Disable FindBoost in CMake" FORCE)

add_library(Boost::boost INTERFACE IMPORTED GLOBAL)
set_target_properties(Boost::boost PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/third_party/boost_1_84_0")

add_subdirectory(dynarmic)

file(GLOB_RECURSE SOURCE_FILES CONFIGURE_DEPENDS src/*.c src/*.cpp)

add_executable(sprout ${SOURCE_FILES} ${CMAKE_SOURCE_DIR}/stb/stb_vorbis.c src/sprout.rc)
target_include_directories(sprout PUBLIC include/)
target_link_libraries(sprout ${CMAKE_DL_LIBS} zlibstatic SDL2::SDL2 SDL2main glad dynarmic)
set_target_properties(sprout PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

if (MINGW)
    target_link_options(sprout PRIVATE -static)
endif()

# Launcher (native Win32 GUI) - MSVC only, no external dependencies
if(MSVC)
    add_executable(launcher
        launcher/main.cpp
        launcher/scanner.cpp
        launcher/config.cpp
        launcher/controls_dialog.cpp
        src/actions.c
        launcher/scanner.h
        launcher/gamepad_icon.h
        launcher/widgets.h
        launcher/config.h
        launcher/controls_dialog.h
    )
    target_include_directories(launcher PRIVATE include/ launcher/)
    target_link_libraries(launcher PRIVATE user32 gdi32 comctl32 comdlg32 dwmapi shell32 msimg32 winhttp)
    set_target_properties(launcher PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        WIN32_EXECUTABLE TRUE
    )
endif()

# Tests (host executable)
add_executable(test_actions tests/test_actions.c src/actions.c)
target_include_directories(test_actions PRIVATE include/)
set_target_properties(test_actions PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

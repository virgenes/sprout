cmake_minimum_required(VERSION 3.1)
project(pvz2native)

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

# PvZ2 is armeabi-v7a (32-bit ARM) only -- no need for the A64 frontend.
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "Selects which frontends to enable" FORCE)
add_subdirectory(dynarmic)

add_subdirectory(pvz2native)

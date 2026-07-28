#ifndef SPROUT_ELF32_LOADER_H
#define SPROUT_ELF32_LOADER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bytes allocated past mem_size so a wide, unaligned access in the last guest
 * page cannot run off the end of the buffer -- see the calloc in
 * pvz2_elf_load() and build_page_table() in pvz2_run_test.cpp. */
#define PVZ2_MEM_GUARD_SLACK 4096u

/* A flat emulated 32-bit ARM address space, plus enough bookkeeping to
 * resolve exported symbols by name (to find JNI entry points) and to
 * dispatch imported symbols to host trampolines (see trampoline_names). */
typedef struct {
    uint8_t *mem;              /* flat buffer covering the whole emulated address space */
    uint32_t mem_size;

    uint32_t so_base;          /* load bias: where libPVZ2.so's segments were placed in mem */
    uint32_t so_span;          /* highest (vaddr+memsz) among PT_LOAD segments, unbiased */
    uint32_t so_entry;         /* e_entry + so_base (mostly irrelevant for a .so) */

    /* dynamic symbol table, kept around for pvz2_elf_find_symbol() */
    const uint8_t *dynsym;     /* points into mem */
    uint32_t dynsym_count;
    const char *dynstr;        /* points into mem */

    /* One synthetic "SVC #index" stub per unique unresolved imported symbol.
     * trampoline_names[index] is the imported symbol's name (owned, malloc'd). */
    uint32_t trampoline_base;
    uint32_t trampoline_count;
    uint32_t trampoline_capacity;
    char **trampoline_names;

    /* PT_ARM_EXIDX: the ARM exception index table (unbiased vaddr + byte
     * size), needed by __gnu_Unwind_Find_exidx so C++ throws can unwind. */
    uint32_t exidx_vaddr;
    uint32_t exidx_size;

    /* Imported symbols of type STT_OBJECT: data the guest READS, so they get
     * a block of real guest memory each rather than an SVC trampoline (which
     * the guest would read as an instruction word). data_import_addrs[i] is
     * where data_import_names[i] lives; dependency modules populate the ones
     * whose contents matter -- notably bionic's ctype tables. */
    uint32_t data_import_count;
    char *data_import_names[16];
    uint32_t data_import_addrs[16];

    /* DT_INIT_ARRAY: table of C++ static/global constructor function
     * pointers the ELF loader normally calls automatically at load time
     * (before any other code runs). Entries are already so_base-relocated
     * by the time pvz2_elf_load() returns (covered by DT_REL like any other
     * data word). Caller is responsible for actually invoking them (see
     * run_init_array() in pvz2_run_test.cpp) -- pvz2_elf_load() only loads
     * and relocates, it doesn't execute guest code itself. */
    uint32_t init_array_vaddr;   /* 0 if absent */
    uint32_t init_array_count;   /* number of 4-byte function pointers */
} pvz2_elf_image_t;

/* Loads and fully relocates libPVZ2.so (or any armeabi-v7a ET_DYN .so) into
 * a freshly allocated flat buffer. Returns 0 on success.
 * space_size is the total size of the emulated address space to allocate
 * (must be large enough for the .so image + trampoline table + stack). */
int pvz2_elf_load(const char *path, uint32_t space_size, pvz2_elf_image_t *out);

/* Looks up a defined (exported) symbol's address in the emulated address
 * space by name, e.g. "Java_com_ea_EAThread_EAThread_Init". Returns 0 if
 * not found or not defined (SHN_UNDEF) in this image. */
uint32_t pvz2_elf_find_symbol(const pvz2_elf_image_t *img, const char *name);

/* Mints an additional "SVC #index" stub after loading, for guest-callable host
 * code that corresponds to no imported symbol -- specifically the OpenSL ES
 * interface vtables, whose entries the guest calls through
 * `(*obj)->Method(obj, ...)` and which therefore must be real guest addresses.
 * Returns the stub's address, or 0 if the table is full. `name` is only used
 * for tracing. The caller must bind a handler to the matching index; see
 * make_guest_callback() in dependency.h, which does both. */
uint32_t pvz2_elf_add_trampoline(pvz2_elf_image_t *img, const char *name);

void pvz2_elf_free(pvz2_elf_image_t *img);

#ifdef __cplusplus
}
#endif

#endif

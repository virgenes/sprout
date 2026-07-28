#include <sprout/elf32/elf32_loader.h>
#include <sprout/elf32/elf32_defs.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Where libPVZ2.so's segments get placed inside the emulated address space,
 * and where we put the synthetic "SVC #index" import trampolines. Chosen so
 * everything fits comfortably below a modest space_size (see caller). */
#define PVZ2_SO_BASE        0x00100000u
#define PVZ2_TRAMPOLINE_BASE 0x00001000u
#define PVZ2_TRAMPOLINE_MAX  4096u

/* Undefined symbols of type STT_OBJECT are DATA, not code: the guest reads
 * through them instead of calling them. libPVZ2.so has ten -- __sF,
 * __stack_chk_guard, the three bionic ctype tables (_ctype_, _tolower_tab_,
 * _toupper_tab_) and five OpenSLES SL_IID_* interface ids.
 *
 * Pointing those at an SVC trampoline means the guest reads the instruction
 * word (0xEF0000xx) as if it were the datum. For the ctype tables that
 * silently corrupts every isalpha/isdigit/tolower in the engine, with no
 * error anywhere -- so they get real, writable guest memory instead, which
 * the dependency modules then fill in. */
#define PVZ2_DATA_IMPORT_BASE 0x0000B000u
#define PVZ2_DATA_IMPORT_SLOT 1024u
#define PVZ2_DATA_IMPORT_MAX  16u

static uint32_t read32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static void write32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, sizeof(v));
}

static char *dup_str(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = (char *)malloc(len);
    if (copy) {
        memcpy(copy, s, len);
    }
    return copy;
}

/* Finds (or creates) the trampoline stub for an imported symbol name and
 * returns its address in the emulated address space. */
static uint32_t get_or_create_trampoline(pvz2_elf_image_t *img, const char *name) {
    for (uint32_t i = 0; i < img->trampoline_count; ++i) {
        if (strcmp(img->trampoline_names[i], name) == 0) {
            return img->trampoline_base + i * 4;
        }
    }
    if (img->trampoline_count >= img->trampoline_capacity) {
        fprintf(stderr, "elf32_loader: trampoline table full (max %u), cannot import '%s'\n",
                img->trampoline_capacity, name);
        return 0;
    }
    uint32_t idx = img->trampoline_count++;
    img->trampoline_names[idx] = dup_str(name);
    write32(img->mem + img->trampoline_base + idx * 4, 0xEF000000u | (idx & 0x00FFFFFFu)); /* SVC #idx */
    return img->trampoline_base + idx * 4;
}

/* Public wrapper -- see the header. Deduplication by name is what we want here
 * too: every OpenSL vtable slot gets its own unique name, so each ends up with
 * its own index, which is how the handler knows which method was called. */
uint32_t pvz2_elf_add_trampoline(pvz2_elf_image_t *img, const char *name) {
    if (img == NULL || name == NULL) return 0;
    return get_or_create_trampoline(img, name);
}

/* Resolves a symbol reference (relocation target) to an address in the
 * emulated address space: either the symbol's own definition within this
 * .so, or a fresh/reused import trampoline if it's undefined (external). */
/* Gives an imported DATA symbol its own block of real guest memory (see
 * PVZ2_DATA_IMPORT_BASE). The block starts zeroed; dependency modules fill in
 * the ones whose contents matter. */
static uint32_t get_or_create_data_import(pvz2_elf_image_t *img, const char *name) {
    for (uint32_t i = 0; i < img->data_import_count; ++i) {
        if (strcmp(img->data_import_names[i], name) == 0) {
            return PVZ2_DATA_IMPORT_BASE + i * PVZ2_DATA_IMPORT_SLOT;
        }
    }
    if (img->data_import_count >= PVZ2_DATA_IMPORT_MAX) {
        fprintf(stderr, "elf32_loader: data-import table full, cannot import '%s'\n", name);
        return 0;
    }
    uint32_t idx = img->data_import_count++;
    img->data_import_names[idx] = dup_str(name);
    img->data_import_addrs[idx] = PVZ2_DATA_IMPORT_BASE + idx * PVZ2_DATA_IMPORT_SLOT;
    return img->data_import_addrs[idx];
}

static uint32_t resolve_symbol(pvz2_elf_image_t *img, uint32_t symidx) {
    const Elf32_Sym *sym = (const Elf32_Sym *)(img->dynsym) + symidx;
    if (sym->st_shndx != SHN_UNDEF) {
        return img->so_base + sym->st_value;
    }
    const char *name = img->dynstr + sym->st_name;
    if (ELF32_ST_TYPE(sym->st_info) == STT_OBJECT) {
        return get_or_create_data_import(img, name);
    }
    return get_or_create_trampoline(img, name);
}

static void apply_relocations(pvz2_elf_image_t *img, uint32_t rel_vaddr, uint32_t rel_size) {
    if (rel_vaddr == 0 || rel_size == 0) {
        return;
    }
    const Elf32_Rel *rel = (const Elf32_Rel *)(img->mem + img->so_base + rel_vaddr);
    uint32_t count = rel_size / (uint32_t)sizeof(Elf32_Rel);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t type = ELF32_R_TYPE(rel[i].r_info);
        uint32_t symidx = ELF32_R_SYM(rel[i].r_info);
        uint8_t *target = img->mem + img->so_base + rel[i].r_offset;

        switch (type) {
            case R_ARM_RELATIVE: {
                uint32_t addend = read32(target);
                write32(target, addend + img->so_base);
                break;
            }
            case R_ARM_ABS32: {
                uint32_t addend = read32(target);
                uint32_t sym_addr = resolve_symbol(img, symidx);
                write32(target, addend + sym_addr);
                break;
            }
            case R_ARM_GLOB_DAT:
            case R_ARM_JUMP_SLOT: {
                uint32_t sym_addr = resolve_symbol(img, symidx);
                write32(target, sym_addr);
                break;
            }
            case R_ARM_COPY:
                fprintf(stderr, "elf32_loader: unexpected R_ARM_COPY relocation, skipping\n");
                break;
            default:
                fprintf(stderr, "elf32_loader: unhandled relocation type %u at offset 0x%08x\n",
                        type, rel[i].r_offset);
                break;
        }
    }
}

int pvz2_elf_load(const char *path, uint32_t space_size, pvz2_elf_image_t *out) {
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "elf32_loader: cannot open '%s'\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_size <= 0) {
        fprintf(stderr, "elf32_loader: empty or unreadable file '%s'\n", path);
        fclose(f);
        return -1;
    }
    uint8_t *file_buf = (uint8_t *)malloc((size_t)file_size);
    if (!file_buf || fread(file_buf, 1, (size_t)file_size, f) != (size_t)file_size) {
        fprintf(stderr, "elf32_loader: failed to read '%s'\n", path);
        free(file_buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)file_buf;
    if ((size_t)file_size < sizeof(Elf32_Ehdr) ||
        eh->e_ident[EI_MAG0] != 0x7f || eh->e_ident[EI_MAG1] != 'E' ||
        eh->e_ident[EI_MAG2] != 'L' || eh->e_ident[EI_MAG3] != 'F') {
        fprintf(stderr, "elf32_loader: '%s' is not an ELF file\n", path);
        free(file_buf);
        return -1;
    }
    if (eh->e_ident[EI_CLASS] != ELFCLASS32 || eh->e_machine != EM_ARM) {
        fprintf(stderr, "elf32_loader: '%s' is not a 32-bit ARM ELF (class=%d machine=%d)\n",
                path, eh->e_ident[EI_CLASS], eh->e_machine);
        free(file_buf);
        return -1;
    }
    if (eh->e_type != ET_DYN && eh->e_type != ET_EXEC) {
        fprintf(stderr, "elf32_loader: '%s' has unsupported e_type %d\n", path, eh->e_type);
        free(file_buf);
        return -1;
    }

    const Elf32_Phdr *phdrs = (const Elf32_Phdr *)(file_buf + eh->e_phoff);

    uint32_t so_span = 0;
    const Elf32_Phdr *dyn_phdr = NULL;
    for (int i = 0; i < eh->e_phnum; ++i) {
        const Elf32_Phdr *ph = &phdrs[i];
        if (ph->p_type == PT_LOAD) {
            uint32_t end = ph->p_vaddr + ph->p_memsz;
            if (end > so_span) so_span = end;
        } else if (ph->p_type == PT_ARM_EXIDX) {
            /* The ARM exception index table. __gnu_Unwind_Find_exidx needs its
             * address and entry count to unwind C++ exceptions; without them
             * a throw cannot find its handler. */
            out->exidx_vaddr = ph->p_vaddr;
            out->exidx_size = ph->p_filesz;
        } else if (ph->p_type == PT_DYNAMIC) {
            dyn_phdr = ph;
        }
    }
    if (!dyn_phdr) {
        fprintf(stderr, "elf32_loader: '%s' has no PT_DYNAMIC segment\n", path);
        free(file_buf);
        return -1;
    }

    uint32_t trampoline_bytes = PVZ2_TRAMPOLINE_MAX * 4;
    uint32_t required = PVZ2_SO_BASE + so_span;
    if (required < PVZ2_TRAMPOLINE_BASE + trampoline_bytes) {
        required = PVZ2_TRAMPOLINE_BASE + trampoline_bytes;
    }
    if (space_size < required) {
        fprintf(stderr, "elf32_loader: space_size 0x%08x too small, need at least 0x%08x\n",
                space_size, required);
        free(file_buf);
        return -1;
    }

    /* One page of slack past mem_size. The JIT reaches guest memory through a
     * page table (see build_page_table in pvz2_run_test.cpp), which resolves an
     * access to `mem + vaddr` and reads the full width from there without
     * re-checking the end of the buffer -- so an unaligned 8-byte load in the
     * very last guest page would read a few bytes past the allocation. The
     * slack absorbs that; mem_size still describes the addressable space. */
    out->mem = (uint8_t *)calloc(1, (size_t)space_size + PVZ2_MEM_GUARD_SLACK);
    if (!out->mem) {
        fprintf(stderr, "elf32_loader: failed to allocate %u bytes of emulated address space\n", space_size);
        free(file_buf);
        return -1;
    }
    out->mem_size = space_size;
    out->so_base = PVZ2_SO_BASE;
    out->so_span = so_span;
    out->so_entry = PVZ2_SO_BASE + eh->e_entry;
    out->trampoline_base = PVZ2_TRAMPOLINE_BASE;
    out->trampoline_capacity = PVZ2_TRAMPOLINE_MAX;
    out->trampoline_names = (char **)calloc(PVZ2_TRAMPOLINE_MAX, sizeof(char *));

    /* Reserve trampoline index 0 as a "$halt" sentinel: callers can point an
     * emulated LR at (trampoline_base + 0) to detect "the guest function
     * returned" via CallSVC(0), without it colliding with a real import. */
    out->trampoline_names[0] = dup_str("$halt");
    out->trampoline_count = 1;
    write32(out->mem + out->trampoline_base, 0xEF000000u);

    for (int i = 0; i < eh->e_phnum; ++i) {
        const Elf32_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) continue;
        memcpy(out->mem + out->so_base + ph->p_vaddr, file_buf + ph->p_offset, ph->p_filesz);
        /* remaining bytes up to p_memsz (BSS) are already zero from calloc */
    }

    /* Walk PT_DYNAMIC to find the pieces we need. */
    uint32_t strtab_vaddr = 0, symtab_vaddr = 0;
    uint32_t rel_vaddr = 0, rel_size = 0;
    uint32_t jmprel_vaddr = 0, jmprel_size = 0;
    uint32_t hash_vaddr = 0;
    uint32_t init_array_vaddr = 0, init_array_size = 0;

    const Elf32_Dyn *dyn = (const Elf32_Dyn *)(out->mem + out->so_base + dyn_phdr->p_vaddr);
    uint32_t dyn_count = dyn_phdr->p_memsz / (uint32_t)sizeof(Elf32_Dyn);
    for (uint32_t i = 0; i < dyn_count && dyn[i].d_tag != DT_NULL; ++i) {
        switch (dyn[i].d_tag) {
            case DT_STRTAB: strtab_vaddr = dyn[i].d_un.d_val; break;
            case DT_SYMTAB: symtab_vaddr = dyn[i].d_un.d_val; break;
            case DT_REL:    rel_vaddr    = dyn[i].d_un.d_val; break;
            case DT_RELSZ:  rel_size     = dyn[i].d_un.d_val; break;
            case DT_JMPREL: jmprel_vaddr = dyn[i].d_un.d_val; break;
            case DT_PLTRELSZ: jmprel_size = dyn[i].d_un.d_val; break;
            case DT_HASH:   hash_vaddr   = dyn[i].d_un.d_val; break;
            case DT_INIT_ARRAY:   init_array_vaddr = dyn[i].d_un.d_val; break;
            case DT_INIT_ARRAYSZ: init_array_size  = dyn[i].d_un.d_val; break;
            default: break;
        }
    }

    if (!strtab_vaddr || !symtab_vaddr) {
        fprintf(stderr, "elf32_loader: '%s' missing DT_STRTAB/DT_SYMTAB\n", path);
        free(file_buf);
        pvz2_elf_free(out);
        return -1;
    }

    out->dynstr = (const char *)(out->mem + out->so_base + strtab_vaddr);
    out->dynsym = out->mem + out->so_base + symtab_vaddr;

    if (hash_vaddr) {
        const uint32_t *hash = (const uint32_t *)(out->mem + out->so_base + hash_vaddr);
        out->dynsym_count = hash[1]; /* nchain == number of dynamic symbols, by convention */
    } else {
        fprintf(stderr, "elf32_loader: '%s' has no DT_HASH (GNU hash only?), symbol lookups by name may miss entries\n", path);
        out->dynsym_count = 0;
    }

    apply_relocations(out, rel_vaddr, rel_size);
    apply_relocations(out, jmprel_vaddr, jmprel_size);

    /* .init_array entries are R_ARM_RELATIVE-relocated data words like any
     * other, so they're already so_base-biased by the apply_relocations()
     * calls above -- safe to read as final guest addresses now. */
    out->init_array_vaddr = init_array_vaddr;
    out->init_array_count = init_array_size / 4u;

    free(file_buf);

    printf("elf32_loader: loaded '%s' at base=0x%08x span=0x%x entry=0x%08x symbols=%u imports=%u init_array=%u\n",
           path, out->so_base, out->so_span, out->so_entry, out->dynsym_count, out->trampoline_count,
           out->init_array_count);

    return 0;
}

uint32_t pvz2_elf_find_symbol(const pvz2_elf_image_t *img, const char *name) {
    const Elf32_Sym *syms = (const Elf32_Sym *)img->dynsym;
    for (uint32_t i = 1; i < img->dynsym_count; ++i) { /* index 0 is always the null symbol */
        if (syms[i].st_shndx == SHN_UNDEF) continue;
        const char *sym_name = img->dynstr + syms[i].st_name;
        if (strcmp(sym_name, name) == 0) {
            return img->so_base + syms[i].st_value;
        }
    }
    return 0;
}

void pvz2_elf_free(pvz2_elf_image_t *img) {
    if (img->trampoline_names) {
        for (uint32_t i = 0; i < img->trampoline_count; ++i) {
            free(img->trampoline_names[i]);
        }
        free(img->trampoline_names);
    }
    free(img->mem);
    memset(img, 0, sizeof(*img));
}

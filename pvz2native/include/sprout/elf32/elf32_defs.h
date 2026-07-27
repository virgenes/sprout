#ifndef SPROUT_ELF32_DEFS_H
#define SPROUT_ELF32_DEFS_H

/* Minimal ELF32 structures/constants, defined locally so we don't depend on
 * a system <elf.h> (not reliably available on Windows/MinGW). Layouts match
 * the standard ELF32 ABI verbatim -- see System V ABI / Linux Standard Base. */

#include <stdint.h>

typedef uint32_t Elf32_Addr;
typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Off;
typedef int32_t  Elf32_Sword;
typedef uint32_t Elf32_Word;

#define EI_NIDENT 16

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf32_Half e_type;
    Elf32_Half e_machine;
    Elf32_Word e_version;
    Elf32_Addr e_entry;
    Elf32_Off  e_phoff;
    Elf32_Off  e_shoff;
    Elf32_Word e_flags;
    Elf32_Half e_ehsize;
    Elf32_Half e_phentsize;
    Elf32_Half e_phnum;
    Elf32_Half e_shentsize;
    Elf32_Half e_shnum;
    Elf32_Half e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
} Elf32_Phdr;

typedef struct {
    Elf32_Sword d_tag;
    union {
        Elf32_Word d_val;
        Elf32_Addr d_ptr;
    } d_un;
} Elf32_Dyn;

typedef struct {
    Elf32_Word    st_name;
    Elf32_Addr    st_value;
    Elf32_Word    st_size;
    unsigned char st_info;
    unsigned char st_other;
    Elf32_Half    st_shndx;
} Elf32_Sym;

typedef struct {
    Elf32_Addr r_offset;
    Elf32_Word r_info;
} Elf32_Rel;

#define ELF32_R_SYM(info)  ((info) >> 8)
#define ELF32_R_TYPE(info) ((unsigned char)(info))

/* e_ident indices */
#define EI_MAG0    0
#define EI_MAG1    1
#define EI_MAG2    2
#define EI_MAG3    3
#define EI_CLASS   4
#define EI_DATA    5

#define ELFCLASS32 1
#define ELFDATA2LSB 1

/* e_type */
#define ET_EXEC 2
#define ET_DYN  3

/* e_machine */
#define EM_ARM 40

/* p_type */
#define PT_LOAD    1
#define PT_DYNAMIC 2

/* d_tag */
#define DT_NULL      0
#define DT_NEEDED    1
#define DT_PLTRELSZ  2
#define DT_HASH      4
#define DT_STRTAB    5
#define DT_SYMTAB    6
#define DT_RELA      7
#define DT_STRSZ     10
#define DT_SYMENT    11
#define DT_INIT      12
#define DT_FINI      13
#define DT_REL       17
#define DT_RELSZ     18
#define DT_RELENT    19
#define DT_PLTREL    20
#define DT_JMPREL    23
#define DT_INIT_ARRAY   25
#define DT_INIT_ARRAYSZ 27

/* ARM relocation types (REL, not RELA -- addend lives in the target word) */
#define R_ARM_ABS32     2
#define R_ARM_REL32     3
#define R_ARM_COPY      20
#define R_ARM_GLOB_DAT  21
#define R_ARM_JUMP_SLOT 22
#define R_ARM_RELATIVE  23

#define PT_ARM_EXIDX 0x70000001

#define SHN_UNDEF 0

/* st_info symbol types. STT_OBJECT marks an imported DATUM the guest reads
 * through, as opposed to STT_FUNC which it calls -- the two need completely
 * different treatment when the symbol is undefined. */
#define ELF32_ST_TYPE(info) ((info) & 0xF)
#define STT_NOTYPE 0
#define STT_OBJECT 1
#define STT_FUNC   2

#endif

// Format of an ELF executable file
#ifndef __KERNEL_ELF_H
#define __KERNEL_ELF_H

#define ELF_MAGIC 0x464C457FU // "\x7FELF" in little endian

// ELF object file types (e_type)
#define ET_NONE 0 // No file type
#define ET_REL  1 // Relocatable file
#define ET_EXEC 2 // Executable file
#define ET_DYN  3 // Shared object / PIE executable

// File header
struct elfhdr {
    uint magic; // must equal ELF_MAGIC
    uchar elf[12];
    ushort type;
    ushort machine;
    uint version;
    uint64 entry;
    uint64 phoff;
    uint64 shoff;
    uint flags;
    ushort ehsize;
    ushort phentsize;
    ushort phnum;
    ushort shentsize;
    ushort shnum;
    ushort shstrndx;
};

// Program section header
struct proghdr {
    uint32 type;
    uint32 flags;
    uint64 off;
    uint64 vaddr;
    uint64 paddr;
    uint64 filesz;
    uint64 memsz;
    uint64 align;
};

// Values for Proghdr type
#define ELF_PROG_LOAD      1 // Loadable segment
#define ELF_PROG_DYNAMIC   2 // Dynamic linking information
#define ELF_PROG_INTERP    3 // Interpreter path
#define ELF_PROG_NOTE      4 // Auxiliary information
#define ELF_PROG_PHDR      6 // Program header table
#define ELF_PROG_TLS       7 // Thread-Local Storage
#define ELF_PROG_GNU_RELRO 0x6474e552 // Read-only after relocation

// Flag bits for Proghdr flags
#define ELF_PROG_FLAG_EXEC  1
#define ELF_PROG_FLAG_WRITE 2
#define ELF_PROG_FLAG_READ  4

// ELF page alignment helpers for loading
#define ELF_PAGESTART(x)  ((x) & ~((uint64)(PGSIZE - 1)))
#define ELF_PAGEOFFSET(x) ((x) & ((uint64)(PGSIZE - 1)))
#define ELF_PAGEALIGN(x)  (((x) + PGSIZE - 1) & ~((uint64)(PGSIZE - 1)))

// Maximum interpreter path length
#define ELF_INTERP_MAXLEN 256

/*
 * Auxiliary vector types (AT_*) — passed on the user stack after envp[].
 * The dynamic linker (ld.so / ld-musl) uses these to locate the main
 * executable's program headers and entry point.
 */
#define AT_NULL     0  // End of auxiliary vector
#define AT_IGNORE   1  // Ignore entry
#define AT_EXECFD   2  // File descriptor of program
#define AT_PHDR     3  // Program headers for program
#define AT_PHENT    4  // Size of program header entry
#define AT_PHNUM    5  // Number of program headers
#define AT_PAGESZ   6  // System page size
#define AT_BASE     7  // Base address of interpreter
#define AT_FLAGS    8  // Flags
#define AT_ENTRY    9  // Entry point of program
#define AT_NOTELF  10  // Program is not ELF
#define AT_UID     11  // Real uid
#define AT_EUID    12  // Effective uid
#define AT_GID     13  // Real gid
#define AT_EGID    14  // Effective gid
#define AT_PLATFORM 15 // String identifying CPU for optimizations
#define AT_HWCAP   16  // Arch dependent hints at CPU capabilities
#define AT_CLKTCK  17  // Frequency at which times() increments
#define AT_SECURE  23  // Boolean, was exec setuid-like?
#define AT_RANDOM  25  // Address of 16 random bytes
#define AT_HWCAP2  26  // More arch-specific hints
#define AT_EXECFN  31  // Filename of program
#define AT_SYSINFO_EHDR 33 // vDSO ELF header

#endif /* __KERNEL_ELF_H */

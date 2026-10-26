/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/* libbacktrace's public entry point, backtrace_create_state(filename, ...),
   treats `filename` as THE EXECUTABLE: on the first lookup it reads that file
   as the main program and discovers every other loaded module once, through
   dl_iterate_phdr, re-opening each by the name the loader has for it. That
   rules out the obvious port of the resolver's per-object design:
     - a plugin's .so or a position-independent executable given as `filename`
       is not registered at its own load base;
     - a plugin opened by a relative path and then chdir()ed away from cannot
       be re-opened by that name (DlopenPluginTest covers exactly that);
     - a library dlopen()ed after the first lookup is unknown to the state.
   The resolver therefore keeps its own per-object cache (keyed by the path and
   load base it already finds through dladdr and /proc/self/maps) and
   registers each object itself, which needs one internal function of
   libbacktrace: elf_add(), the per-file registration behind both its own
   executable path and its dl_iterate_phdr callback. elf_add() is static in
   elf.c, so this file INCLUDES elf.c — libbacktrace's sources are not
   modified — and is compiled in its place (see third_party/libbacktrace/
   CMakeLists.txt). The dependency is on elf_add()'s signature, which may
   change in a later libbacktrace; the copy is pinned to one commit, so
   nothing changes until it is updated deliberately, and if the signature
   changes then, this file fails to compile instead of misbehaving.

   The cleaner long-term route is a public backtrace_add_module() in
   libbacktrace itself, which would leave only the layout check below in
   this file. */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Every file elf.c opens — the object file, a separate debug file found by
   its build ID or .gnu_debuglink, a dwz alt file named by .gnu_debugaltlink —
   is opened by cslg_open_checked() below instead of libbacktrace's own
   backtrace_open() (posix.c, still used by the rest of libbacktrace). */
#define backtrace_open cslg_open_checked
#include "elf.c"
#undef backtrace_open

/* backtrace_create_state()'s flag for a state used by several threads (its
   FLAGS argument is a bit mask; libbacktrace has no name for the bit). */
#define CSLG_BACKTRACE_THREADED 1

/* Sanity bound on the number of section headers a file may claim before the
   layout check maps them. Real object files have a few dozen; the bound only
   keeps a corrupt count from mapping an absurd range. */
#define CSLG_MAX_SECTION_HEADERS 65536

/* The size of an ELF note header: namesz, descsz and type, 4 bytes each. */
#define CSLG_NOTE_HEADER_SIZE 12

/* The section in which MiniDebugInfo — an xz-compressed ELF file holding the
   symbol table of the functions .dynsym lacks, what Fedora leaves in its
   stripped binaries — travels. libbacktrace reads it. */
#define CSLG_MINIDEBUGINFO_SECTION ".gnu_debugdata"

/* What cslg_open_checked() learned from the file it opened last: its
   directory (after symbolic links, as the kernel names the open file) and
   whether it carries a symbol table of its own. One instance is enough:
   every registration runs under the resolver's mutex (see the header). */
static struct {
    char directory[PATH_MAX];
    int has_own_symtab;
} cslg_last_open;

/* Whether every note in the SHT_NOTE section `shdr` fits the section: its
   header, its name and its descriptor, padded to the section's alignment
   (4, or 8 for sections aligned to 8) as readelf reads them. elf_add() reads
   the build-ID note's descriptor with a size test that is the wrong way
   round, so a note that claims more bytes than its section holds would be
   read far beyond it. */
static int cslg_notes_fit_section(int descriptor, const b_elf_shdr* shdr,
                                  backtrace_error_callback error_callback, void* data) {
    const uint64_t size = shdr->sh_size;
    const uint64_t align = shdr->sh_addralign == 8 ? 8 : 4;
    struct backtrace_view view;
    uint64_t offset = 0;
    int ok = 1;

    if (size == 0) {
        return 1;
    }
    if (size < CSLG_NOTE_HEADER_SIZE) {
        return 0;
    }
    if (!backtrace_get_view(NULL, descriptor, (off_t)shdr->sh_offset, size, error_callback, data, &view)) {
        return 0;
    }
    while (size - offset >= CSLG_NOTE_HEADER_SIZE) {
        const unsigned char* note = (const unsigned char*)view.data + offset;
        uint32_t namesz;
        uint32_t descsz;
        uint64_t desc_offset;
        uint64_t end;

        memcpy(&namesz, note, sizeof namesz);
        memcpy(&descsz, note + sizeof namesz, sizeof descsz);
        desc_offset = (CSLG_NOTE_HEADER_SIZE + (uint64_t)namesz + align - 1) / align * align;
        if (desc_offset > size - offset || descsz > size - offset - desc_offset) {
            ok = 0;
            break;
        }
        end = (desc_offset + descsz + align - 1) / align * align;
        if (end >= size - offset) {
            break;
        }
        offset += end;
    }
    backtrace_release_view(NULL, &view, error_callback, data);
    return ok;
}

/* Whether one of the `shnum` section headers in `shdrs` names `name`, read
   from the section-name table at index `shstrndx` (both already checked by
   the caller). */
static int cslg_has_section_named(int descriptor, const b_elf_shdr* shdrs, uint64_t shnum,
                                  uint64_t shstrndx, const char* name,
                                  backtrace_error_callback error_callback, void* data) {
    const b_elf_shdr* names_shdr = &shdrs[shstrndx];
    const uint64_t name_size = strlen(name) + 1;
    struct backtrace_view names;
    uint64_t i;
    int found = 0;

    if (names_shdr->sh_type == SHT_NOBITS || names_shdr->sh_size == 0) {
        return 0;
    }
    if (!backtrace_get_view(NULL, descriptor, (off_t)names_shdr->sh_offset, names_shdr->sh_size,
                            error_callback, data, &names)) {
        return 0;
    }
    for (i = 1; i < shnum && !found; ++i) {
        const uint64_t offset = shdrs[i].sh_name;
        found = offset < names_shdr->sh_size && names_shdr->sh_size - offset >= name_size
                && memcmp((const char*)names.data + offset, name, name_size) == 0;
    }
    backtrace_release_view(NULL, &names, error_callback, data);
    return found;
}

/* libbacktrace maps file ranges with mmap and never compares them with the
   file's size: the section header table or a debug section of a TRUNCATED
   file lies beyond the end of the file, is mapped without complaint and
   raises SIGBUS when first read. (The loader only needs the PT_LOAD segments
   at the front, so such a file can still be dlopen()ed — a half-copied
   plugin, a build interrupted mid-write.) Rejecting the file here keeps the
   documented degradation — "<could not open object file>" — instead of a
   crash in the hook. The same goes for the indexes elf_add() follows without
   a bounds check: the section-name table index (e_shstrndx) and a symbol
   table's string table index (sh_link), and for notes larger than their
   section (see cslg_notes_fit_section). Separate debug files and dwz alt files
   are mapped the same way, so every file elf.c opens is checked, not only the
   object file (see cslg_open_checked). A file that is truncated or rewritten
   AFTER the check, while its views are mapped, can still fault: the check
   cannot cover that. */
/* Also reports, in *has_own_symtab, whether the file carries a symbol table
   of its own, as opposed to only .dynsym: a .symtab section, or MiniDebugInfo
   (CSLG_MINIDEBUGINFO_SECTION). The resolver warns once when the executable
   has neither that nor debug information, because no function in it can be
   named then. */
static int cslg_elf_layout_fits_file(int descriptor, uint64_t file_size,
                                     backtrace_error_callback error_callback, void* data,
                                     int* has_own_symtab) {
    b_elf_ehdr ehdr;
    struct backtrace_view view;
    uint64_t shoff;
    uint64_t shnum;
    uint64_t shstrndx;
    uint64_t shentsize;
    uint64_t i;
    const char* problem = NULL;

    *has_own_symtab = 0;
    if (pread(descriptor, &ehdr, sizeof ehdr, 0) != (ssize_t)sizeof ehdr) {
        error_callback(data, "ELF header beyond end of file", 0);
        return 0;
    }
    shoff = ehdr.e_shoff;
    shnum = ehdr.e_shnum;
    shstrndx = ehdr.e_shstrndx;
    shentsize = ehdr.e_shentsize;
    if (shoff == 0) {
        return 1; /* no section headers at all: elf_add() reports that itself */
    }
    if (shentsize != sizeof(b_elf_shdr) || shoff > file_size || shentsize > file_size - shoff) {
        error_callback(data, "ELF section headers beyond end of file", 0);
        return 0;
    }
    if (shnum == 0 || shstrndx == SHN_XINDEX) {
        /* More than SHN_LORESERVE sections: the count, or the section-name
           table index, is in section header 0. */
        b_elf_shdr shdr0;
        if (pread(descriptor, &shdr0, sizeof shdr0, (off_t)shoff) != (ssize_t)sizeof shdr0) {
            error_callback(data, "ELF section headers beyond end of file", 0);
            return 0;
        }
        if (shnum == 0) {
            shnum = shdr0.sh_size;
        }
        if (shstrndx == SHN_XINDEX) {
            shstrndx = shdr0.sh_link;
            /* elf_add()'s correction for old binutils (see the comment there). */
            if (shstrndx >= shnum && shstrndx >= SHN_LORESERVE + 0x100) {
                shstrndx -= 0x100;
            }
        }
    }
    if (shnum > CSLG_MAX_SECTION_HEADERS || shnum * shentsize > file_size - shoff) {
        error_callback(data, "ELF section headers beyond end of file", 0);
        return 0;
    }
    if (shstrndx == 0 || shstrndx >= shnum) {
        error_callback(data, "ELF section-name table index out of range", 0);
        return 0;
    }
    if (!backtrace_get_view(NULL, descriptor, (off_t)shoff, shnum * shentsize, error_callback, data, &view)) {
        return 0;
    }
    for (i = 0; i < shnum && problem == NULL; ++i) {
        const b_elf_shdr* shdr = (const b_elf_shdr*)view.data + i;
        if (shdr->sh_type == SHT_SYMTAB) {
            *has_own_symtab = 1;
        }
        if ((shdr->sh_type == SHT_SYMTAB || shdr->sh_type == SHT_DYNSYM)
            && (shdr->sh_link == 0 || shdr->sh_link >= shnum)) {
            problem = "ELF symbol table string table index out of range";
        } else if (shdr->sh_type == SHT_NOBITS || shdr->sh_type == SHT_NULL) {
            continue;
        } else if (shdr->sh_offset > file_size || shdr->sh_size > file_size - shdr->sh_offset) {
            problem = "ELF section contents beyond end of file";
        } else if (shdr->sh_type == SHT_NOTE
                   && !cslg_notes_fit_section(descriptor, shdr, error_callback, data)) {
            problem = "ELF note larger than its section";
        }
    }
    if (problem == NULL && !*has_own_symtab) {
        *has_own_symtab = cslg_has_section_named(descriptor, (const b_elf_shdr*)view.data, shnum, shstrndx,
                                                 CSLG_MINIDEBUGINFO_SECTION, error_callback, data);
    }
    backtrace_release_view(NULL, &view, error_callback, data);
    if (problem != NULL) {
        error_callback(data, problem, 0);
        return 0;
    }
    return 1;
}

/* Remembers the directory of the file open on `descriptor`, as the kernel
   names it (/proc/self/fd), so after symbolic links: a debug file found
   through its build ID is opened as /usr/lib/debug/.build-id/xx/yyy.debug, a
   link to the real file elsewhere under /usr/lib/debug. `path` is the name it
   was opened by, the fallback without /proc. */
static void cslg_remember_directory(int descriptor, const char* path) {
    char link[32];
    char target[PATH_MAX];
    ssize_t length;
    char* slash;

    snprintf(link, sizeof link, "/proc/self/fd/%d", descriptor);
    length = readlink(link, target, sizeof target - 1);
    if (length <= 0 || target[0] != '/') {
        length = (ssize_t)strnlen(path, sizeof target);
        if ((size_t)length >= sizeof target || path[0] != '/') {
            cslg_last_open.directory[0] = '\0';
            return;
        }
        memcpy(target, path, (size_t)length);
    }
    target[length] = '\0';
    slash = strrchr(target, '/');
    slash[slash == target ? 1 : 0] = '\0'; /* keep "/" for a file in the root */
    memcpy(cslg_last_open.directory, target, strlen(target) + 1);
}

/* libbacktrace's backtrace_open() with three differences, all on paths that
   come from the files themselves or from directories other users may write:
     - A relative name is taken relative to the directory of the file opened
       last, never to the current directory. Relative names come only from a
       .gnu_debugaltlink inside a separate debug file — which libbacktrace
       registers under the name "" — and such a link is relative to the debug
       file's own directory, as gdb reads it (Fedora's read
       "../../.dwz/<package>"): libbacktrace looked it up from the current
       directory, where it finds nothing or, worse, someone else's file.
     - Only regular files are opened, without waiting (O_NONBLOCK): opening a
       FIFO for reading waits for a writer, and a FIFO planted under the name
       of a debug file would hang the program, the resolver's lock held.
     - The file must pass the layout check above.
   A refused file counts as missing, like a file that does not exist. */
int cslg_open_checked(const char* filename, backtrace_error_callback error_callback, void* data,
                      int* does_not_exist) {
    char resolved[PATH_MAX];
    const char* path = filename;
    struct stat st;
    int descriptor;
    int has_own_symtab = 0;

    if (does_not_exist != NULL) {
        *does_not_exist = 0;
    }
    if (filename[0] != '/') {
        const int length = cslg_last_open.directory[0] == '\0'
                ? -1
                : snprintf(resolved, sizeof resolved, "%s/%s", cslg_last_open.directory, filename);
        if (length < 0 || (size_t)length >= sizeof resolved) {
            if (does_not_exist != NULL) {
                *does_not_exist = 1;
            }
            return -1;
        }
        path = resolved;
    }
    descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (descriptor < 0) {
        if (does_not_exist != NULL && (errno == ENOENT || errno == EACCES || errno == ENOTDIR)) {
            *does_not_exist = 1;
        } else {
            error_callback(data, path, errno);
        }
        return -1;
    }
    if (fstat(descriptor, &st) < 0 || !S_ISREG(st.st_mode)
        || !cslg_elf_layout_fits_file(descriptor, (uint64_t)st.st_size, error_callback, data,
                                      &has_own_symtab)) {
        close(descriptor);
        if (does_not_exist != NULL) {
            *does_not_exist = 1;
        }
        return -1;
    }
    cslg_remember_directory(descriptor, path);
    cslg_last_open.has_own_symtab = has_own_symtab;
    return descriptor;
}

struct backtrace_state* cslg_backtrace_create_state(backtrace_error_callback error_callback, void* data) {
    /* The flags are a bit mask since July 2026; the value 1 meant the same in
       the older boolean parameter. */
    struct backtrace_state* state
            = backtrace_create_state(NULL, CSLG_BACKTRACE_THREADED, error_callback, data);
    if (state == NULL) {
        return NULL;
    }
    /* fileline_initialize() — libbacktrace's lazy discovery of the executable
       and, through dl_iterate_phdr, of every loaded module — runs on the
       first lookup only while fileline_fn is still NULL. Installing the two
       "nothing known" handlers now makes it return at once, so a lookup for
       an address no registered file covers reports no information instead of
       starting the discovery the comment at the top rules out. Both handlers
       are replaced as soon as a registered file brings a symbol table or
       debug information (see cslg_backtrace_add_file). */
    backtrace_atomic_store_pointer(&state->fileline_fn, elf_nodebug);
    backtrace_atomic_store_pointer(&state->syminfo_fn, elf_nosyms);
    return state;
}

int cslg_backtrace_add_file(struct backtrace_state* state, const char* path, uintptr_t load_bias,
                            backtrace_error_callback error_callback, void* data, int* has_symtab,
                            int* has_dwarf, int* has_own_symtab) {
    int does_not_exist = 0;
    int descriptor;
    struct libbacktrace_base_address base_address;
    fileline fileline_fn = NULL;
    int found_sym = 0;
    int found_dwarf = 0;
    int ret;

    *has_symtab = 0;
    *has_dwarf = 0;
    *has_own_symtab = 0;
    /* The object file's own path is absolute (the resolver passes no other):
       it starts the chain of directories relative alt links are read from. */
    cslg_last_open.directory[0] = '\0';
    descriptor = cslg_open_checked(path, error_callback, data, &does_not_exist);
    if (descriptor < 0) {
        return 0;
    }
    *has_own_symtab = cslg_last_open.has_own_symtab;
    /* The base libbacktrace adds to every address it reads from the file is
       the loader's load bias (link_map::l_addr, what dl_iterate_phdr reports
       as dlpi_addr): 0 for a non-PIE executable, the map start for a PIE or a
       shared object. dladdr()'s dli_fbase is the map start in every case and
       would be wrong for the non-PIE executable. */
    memset(&base_address, 0, sizeof base_address);
    base_address.m = load_bias;
    /* exe = 0: the file is registered as a module at the given base, never
       deferred as "a PIE whose base dl_iterate_phdr will supply" (the -1
       return of elf_add is for exe = 1 only). debuginfo = 0: this is the
       loaded file, not a separate debug file. elf_add closes the descriptor
       on every return. */
    ret = elf_add(state, path, descriptor, NULL, 0, base_address, NULL, error_callback, data, &fileline_fn,
                  &found_sym, &found_dwarf, NULL, 0, 0, NULL, 0);
    if (ret <= 0) {
        /* elf_add() reads the symbol table first and chains it into the state
           before it turns to the DWARF data. When the DWARF part fails — an
           unknown DWARF version, a damaged .debug_info, an unreadable separate
           debug file — the symbol table stays registered and answers name
           lookups on its own, which is what libbfd offered in that case. Keep
           the file as a symbols-only module instead of rejecting it. */
        if (!found_sym) {
            return 0;
        }
        found_dwarf = 0;
        fileline_fn = NULL;
    }
    /* What backtrace_initialize() does after its own dl_iterate_phdr pass:
       the symbol tables and DWARF units of every registered file are chained
       inside the state; the per-state lookup functions only have to point at
       the chain walkers once some file contributed to it. The state is
       threaded, so the stores are the atomic ones libbacktrace uses itself. */
    if (found_sym) {
        backtrace_atomic_store_pointer(&state->syminfo_fn, elf_syminfo);
    }
    if (found_dwarf && fileline_fn != NULL) {
        backtrace_atomic_store_pointer(&state->fileline_fn, fileline_fn);
    }
    *has_symtab = found_sym;
    *has_dwarf = found_dwarf;
    return 1;
}

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
   libbacktrace itself; the hand-over of this change carries a proposal. */

#include "config.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "elf.c"

/* libbacktrace maps file ranges with mmap and never compares them with the
   file's size: the section header table or a debug section of a TRUNCATED
   file lies beyond the end of the file, is mapped without complaint and
   raises SIGBUS when first read. (The loader only needs the PT_LOAD segments
   at the front, so such a file can still be dlopen()ed — a half-copied
   plugin, a build interrupted mid-write.) Rejecting the file here keeps the
   documented degradation — "<could not open object file>" — instead of a
   crash in the hook. Separate debug files reached through .gnu_debuglink
   are CRC-checked by libbacktrace over their real size and skipped when
   truncated; a truncated build-ID debug file under /usr/lib/debug is not
   covered. */
/* Also reports, in *has_symtab_section, whether the file carries a .symtab
   section of its own (as opposed to only .dynsym): the resolver warns once
   when the executable has neither that nor debug information, because no
   function in it can be named then. */
static int cslg_elf_layout_fits_file(int descriptor, backtrace_error_callback error_callback, void* data,
                                     int* has_symtab_section) {
    struct stat st;
    b_elf_ehdr ehdr;
    struct backtrace_view view;
    uint64_t file_size;
    uint64_t shoff;
    uint64_t shnum;
    uint64_t shentsize;
    uint64_t i;
    int ok = 1;

    if (fstat(descriptor, &st) < 0) {
        error_callback(data, "fstat", errno);
        return 0;
    }
    file_size = (uint64_t)st.st_size;
    if (pread(descriptor, &ehdr, sizeof ehdr, 0) != (ssize_t)sizeof ehdr) {
        error_callback(data, "ELF header beyond end of file", 0);
        return 0;
    }
    shoff = ehdr.e_shoff;
    shnum = ehdr.e_shnum;
    shentsize = ehdr.e_shentsize;
    if (shoff == 0) {
        return 1; /* no section headers at all: elf_add() reports that itself */
    }
    if (shentsize != sizeof(b_elf_shdr) || shoff > file_size || shentsize > file_size - shoff) {
        error_callback(data, "ELF section headers beyond end of file", 0);
        return 0;
    }
    if (shnum == 0) {
        /* More than SHN_LORESERVE sections: the count is in section header 0. */
        b_elf_shdr shdr0;
        if (pread(descriptor, &shdr0, sizeof shdr0, (off_t)shoff) != (ssize_t)sizeof shdr0) {
            error_callback(data, "ELF section headers beyond end of file", 0);
            return 0;
        }
        shnum = shdr0.sh_size;
    }
    if (shnum > 65536 || shnum * shentsize > file_size - shoff) {
        error_callback(data, "ELF section headers beyond end of file", 0);
        return 0;
    }
    if (!backtrace_get_view(NULL, descriptor, (off_t)shoff, shnum * shentsize, error_callback, data, &view)) {
        return 0;
    }
    for (i = 0; i < shnum; ++i) {
        const b_elf_shdr* shdr = (const b_elf_shdr*)view.data + i;
        if (shdr->sh_type == SHT_SYMTAB) {
            *has_symtab_section = 1;
        }
        if (shdr->sh_type == SHT_NOBITS || shdr->sh_type == SHT_NULL) {
            continue;
        }
        if (shdr->sh_offset > file_size || shdr->sh_size > file_size - shdr->sh_offset) {
            ok = 0;
            break;
        }
    }
    backtrace_release_view(NULL, &view, error_callback, data);
    if (!ok) {
        error_callback(data, "ELF section contents beyond end of file", 0);
    }
    return ok;
}

struct backtrace_state* cslg_backtrace_create_state(backtrace_error_callback error_callback, void* data) {
    /* Flag 1 = THREADED (a bit mask since July 2026; the value 1 meant the
       same in the older boolean parameter). */
    struct backtrace_state* state = backtrace_create_state(NULL, 1, error_callback, data);
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
                            int* has_dwarf, int* has_symtab_section) {
    int does_not_exist = 0;
    int descriptor;
    struct libbacktrace_base_address base_address;
    fileline fileline_fn = NULL;
    int found_sym = 0;
    int found_dwarf = 0;
    int ret;

    *has_symtab = 0;
    *has_dwarf = 0;
    *has_symtab_section = 0;
    descriptor = backtrace_open(path, error_callback, data, &does_not_exist);
    if (descriptor < 0) {
        return 0;
    }
    if (!cslg_elf_layout_fits_file(descriptor, error_callback, data, has_symtab_section)) {
        backtrace_close(descriptor, error_callback, data);
        return 0;
    }
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

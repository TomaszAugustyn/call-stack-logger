/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * Instrumented driver for DamagedObjectTest: the resolver meets object files
 * it cannot read, and addresses no object file covers, and must degrade to
 * the documented texts instead of crashing or hanging. One scenario per run,
 * chosen by the first argument:
 *
 *   deleted    the dlopen plugin is copied to a temporary file, loaded, and
 *              the file is deleted before the first traced call into it —
 *              the mapping's path then ends in " (deleted)" and the open
 *              fails ("<could not open object file>").
 *   truncated  the copy is cut right after its last PT_LOAD segment, so the
 *              loader still maps it but the section headers and the debug
 *              information are gone; libbacktrace would map the missing
 *              bytes and fault on them, the resolver's layout check rejects
 *              the file instead (same text as above, and the program lives).
 *   nomodule   the public resolve() API is handed addresses that belong to
 *              no loaded object (0x1, a heap block); the result is printed.
 *   planted    like "deleted", but a copy of the same plugin is then put
 *              under the name the kernel shows for the deleted mapping,
 *              "<path> (deleted)": it must not be read either.
 *   vdso       a copy of the plugin is planted in a fresh current directory as
 *              "linux-vdso.so.1", the name dladdr() reports for the vdso, and
 *              an address in the vdso is resolved at the plugin function's
 *              offset: the planted file must not be read as the vdso.
 *   dwarf      the version of the copy's first .debug_info unit is set to 99,
 *              a DWARF version no reader knows: the DWARF data cannot be
 *              used, but the symbol table can, so the plugin's functions must
 *              still be named (with "???" for the lines).
 *   shstrndx   the copy's index of its section-name table (e_shstrndx) points
 *              far past the section header table.
 *   buildid    the copy's build-ID note claims a 16 MiB descriptor in a
 *              section of a few dozen bytes.
 *
 * The next three load cslg_debuglink_plugin, whose DWARF data sits in a
 * separate debug file that names a dwz alt file by a relative name:
 *
 *   truncated-alt  a copy of the plugin and of its debug file, and a
 *              truncated ELF file under the alt file's name next to them,
 *              which is also the current directory.
 *   fifo-debuglink a copy of the plugin and a FIFO under the debug file's name
 *              next to it.
 *   fifo-alt   a copy of the plugin and of its debug file, and a FIFO under
 *              the alt file's name in another directory, the current one.
 *
 * Opening a FIFO for reading waits for a writer, which never comes: in the
 * two FIFO cases a resolver that opened it would hang the program.
 */

#include "callStack.h"

#include <cstddef> // for offsetof
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h> // for ElfW
#include <sys/auxv.h> // for getauxval(AT_SYSINFO_EHDR)
#include <sys/stat.h>
#include <unistd.h>

// This translation unit is instrumented. It sticks to C-style I/O and char
// buffers: under Clang, constructing a std::string here would instrument
// constexpr members of libstdc++ that have no out-of-line symbol
// (basic_string::_M_init_local_buf) and the link would fail — the reason the
// library itself is compiled without instrumentation.

namespace {

// Copies a file; returns false on failure.
bool copy_file(const char* from, const char* to) {
    FILE* in = std::fopen(from, "rb");
    FILE* out = in != nullptr ? std::fopen(to, "wb") : nullptr;
    bool ok = out != nullptr;
    char buffer[65536];
    size_t got;
    while (ok && (got = std::fread(buffer, 1, sizeof buffer, in)) > 0) {
        ok = std::fwrite(buffer, 1, got, out) == got;
    }
    if (in != nullptr) {
        std::fclose(in);
    }
    if (out != nullptr) {
        std::fclose(out);
    }
    return ok;
}

// Copies the plugin into a fresh temporary directory; fills `dir` and `copy`.
bool copy_plugin(char* dir, size_t dir_size, char* copy, size_t copy_size) {
    std::snprintf(dir, dir_size, "/tmp/cslg_damaged_XXXXXX");
    if (mkdtemp(dir) == nullptr) {
        std::perror("mkdtemp");
        return false;
    }
    std::snprintf(copy, copy_size, "%s/plugin.so", dir);
    if (!copy_file(DAMAGED_PLUGIN_PATH, copy)) {
        std::perror("copy");
        return false;
    }
    chmod(copy, 0755);
    return true;
}

// The end of the last PT_LOAD segment in the file: everything after it is
// unneeded by the loader (section headers, symbol tables, debug sections).
off_t last_load_end(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (f == nullptr) {
        return 0;
    }
    ElfW(Ehdr) ehdr {};
    if (std::fread(&ehdr, sizeof ehdr, 1, f) != 1) {
        std::fclose(f);
        return 0;
    }
    off_t end = 0;
    for (unsigned i = 0; i < ehdr.e_phnum; ++i) {
        ElfW(Phdr) phdr {};
        if (std::fseek(f, static_cast<long>(ehdr.e_phoff + i * sizeof phdr), SEEK_SET) != 0
            || std::fread(&phdr, sizeof phdr, 1, f) != 1) {
            break;
        }
        const off_t segment_end = static_cast<off_t>(phdr.p_offset + phdr.p_filesz);
        if (phdr.p_type == PT_LOAD && segment_end > end) {
            end = segment_end;
        }
    }
    std::fclose(f);
    return end;
}

// Finds the section called `name` in the open ELF file; fills `found`.
bool find_section(FILE* f, const char* name, ElfW(Shdr)* found) {
    ElfW(Ehdr) ehdr {};
    std::fseek(f, 0, SEEK_SET);
    if (std::fread(&ehdr, sizeof ehdr, 1, f) != 1 || ehdr.e_shoff == 0 || ehdr.e_shstrndx >= ehdr.e_shnum) {
        return false;
    }
    ElfW(Shdr) strings {};
    std::fseek(f, static_cast<long>(ehdr.e_shoff + ehdr.e_shstrndx * sizeof(ElfW(Shdr))), SEEK_SET);
    if (std::fread(&strings, sizeof strings, 1, f) != 1) {
        return false;
    }
    for (unsigned i = 0; i < ehdr.e_shnum; ++i) {
        ElfW(Shdr) shdr {};
        std::fseek(f, static_cast<long>(ehdr.e_shoff + i * sizeof shdr), SEEK_SET);
        if (std::fread(&shdr, sizeof shdr, 1, f) != 1) {
            return false;
        }
        char section_name[32] = {};
        std::fseek(f, static_cast<long>(strings.sh_offset + shdr.sh_name), SEEK_SET);
        if (std::fread(section_name, 1, sizeof section_name - 1, f) > 0
            && std::strcmp(section_name, name) == 0) {
            *found = shdr;
            return true;
        }
    }
    return false;
}

// Overwrites `size` bytes at `offset` of the file at `path`.
bool patch_file(const char* path, long offset, const void* bytes, size_t size) {
    FILE* f = std::fopen(path, "r+b");
    if (f == nullptr) {
        return false;
    }
    const bool done = std::fseek(f, offset, SEEK_SET) == 0 && std::fwrite(bytes, 1, size, f) == size;
    std::fclose(f);
    return done;
}

// Sets the version of the first unit in .debug_info to 99. Returns false when
// the file has no .debug_info section.
bool break_dwarf_version(const char* path) {
    FILE* f = std::fopen(path, "rb");
    ElfW(Shdr) shdr {};
    const bool found = f != nullptr && find_section(f, ".debug_info", &shdr);
    if (f != nullptr) {
        std::fclose(f);
    }
    // 32-bit DWARF unit header: unit_length (4 bytes), then version (2).
    const unsigned char version[2] = { 99, 0 };
    return found && patch_file(path, static_cast<long>(shdr.sh_offset + 4), version, sizeof version);
}

// Points e_shstrndx, the index of the section-name table, far past the
// section header table (the largest ordinary index, SHN_LORESERVE - 1).
bool break_section_name_index(const char* path) {
    const ElfW(Half) index = SHN_LORESERVE - 1;
    return patch_file(path, static_cast<long>(offsetof(ElfW(Ehdr), e_shstrndx)), &index, sizeof index);
}

// Makes the build-ID note claim a descriptor of 16 MiB. Returns false when
// the file has no .note.gnu.build-id section.
bool inflate_build_id_note(const char* path) {
    FILE* f = std::fopen(path, "rb");
    ElfW(Shdr) shdr {};
    const bool found = f != nullptr && find_section(f, ".note.gnu.build-id", &shdr);
    if (f != nullptr) {
        std::fclose(f);
    }
    // Note header: namesz (4 bytes), descsz (4), type (4).
    const ElfW(Word) descsz = 16 * 1024 * 1024;
    const long descsz_offset = static_cast<long>(shdr.sh_offset + sizeof(ElfW(Word)));
    return found && patch_file(path, descsz_offset, &descsz, sizeof descsz);
}

int call_plugin(void* handle) {
    auto entry = reinterpret_cast<int (*)(int)>(dlsym(handle, "dlopen_plugin_entry"));
    if (entry == nullptr) {
        std::fprintf(stderr, "dlsym: %s\n", dlerror());
        return 1;
    }
    std::printf("plugin result=%d\n", entry(5));
    return 0;
}

void print_resolution(const char* label, void* callee, void* caller) {
    const std::optional<instrumentation::ResolvedFrame> frame = instrumentation::resolve(callee, caller);
    if (!frame) {
        std::printf("%s: not loggable\n", label);
        return;
    }
    std::printf("%s: NAME=%s FILE=%s LINE=%s\n", label, frame->callee_function_name.c_str(),
                frame->caller_filename.c_str(), frame->caller_line_number ? "set" : "none");
}

// The three cases of cslg_debuglink_plugin (see the comment at the top).
int load_debuglink_plugin(const char* mode) {
    const bool fifo_debuglink = std::strcmp(mode, "fifo-debuglink") == 0;
    const bool fifo_alt = std::strcmp(mode, "fifo-alt") == 0;
    char dir[] = "/tmp/cslg_debuglink_XXXXXX";
    char other_dir[] = "/tmp/cslg_debuglink_cwd_XXXXXX";
    if (mkdtemp(dir) == nullptr || (fifo_alt && mkdtemp(other_dir) == nullptr)) {
        std::perror("mkdtemp");
        return 2;
    }
    char copy[128];
    char debug_copy[128];
    char planted[128];
    std::snprintf(copy, sizeof copy, "%s/%s", dir, DEBUGLINK_PLUGIN_NAME);
    std::snprintf(debug_copy, sizeof debug_copy, "%s/%s", dir, DEBUGLINK_DEBUG_NAME);
    if (fifo_debuglink) {
        std::snprintf(planted, sizeof planted, "%s", debug_copy);
    } else if (fifo_alt) {
        std::snprintf(planted, sizeof planted, "%s/%s", other_dir, DEBUGLINK_ALT_NAME);
    } else {
        std::snprintf(planted, sizeof planted, "%s/%s", dir, DEBUGLINK_ALT_NAME);
    }
    bool ready = copy_file(DEBUGLINK_PLUGIN_PATH, copy) && chmod(copy, 0755) == 0;
    if (!fifo_debuglink) {
        ready = ready && copy_file(DEBUGLINK_DEBUG_PATH, debug_copy);
    }
    if (fifo_debuglink || fifo_alt) {
        ready = ready && mkfifo(planted, 0600) == 0;
    } else {
        ready = ready && copy_file(DAMAGED_PLUGIN_PATH, planted)
                && truncate(planted, last_load_end(planted) + 64) == 0;
    }
    ready = ready && chdir(fifo_alt ? other_dir : dir) == 0;
    if (!ready) {
        std::perror("setup");
        return 2;
    }
    void* handle = dlopen(copy, RTLD_NOW);
    if (handle == nullptr) {
        std::fprintf(stderr, "dlopen: %s\n", dlerror());
        return 2;
    }
    const int rc = call_plugin(handle);
    unlink(planted);
    unlink(debug_copy);
    unlink(copy);
    rmdir(dir);
    if (fifo_alt) {
        rmdir(other_dir);
    }
    std::puts("DAMAGED_DONE");
    return rc;
}

} // namespace

// Resolves an address of the vdso while a copy of the plugin sits in the
// current directory under the vdso's name. The address is the vdso's base plus
// the offset of dlopen_plugin_entry in the plugin, so a resolver that read the
// planted file would name it after that function.
int resolve_vdso_with_planted_file() {
    char dir[] = "/tmp/cslg_vdso_XXXXXX";
    if (mkdtemp(dir) == nullptr || chdir(dir) != 0 || !copy_file(DAMAGED_PLUGIN_PATH, "linux-vdso.so.1")) {
        std::perror("setup");
        return 2;
    }
    void* plugin = dlopen(DAMAGED_PLUGIN_PATH, RTLD_NOW);
    void* entry = plugin != nullptr ? dlsym(plugin, "dlopen_plugin_entry") : nullptr;
    Dl_info plugin_info {};
    char* vdso = reinterpret_cast<char*>(getauxval(AT_SYSINFO_EHDR));
    if (entry == nullptr || vdso == nullptr || dladdr(entry, &plugin_info) == 0) {
        std::puts("VDSO_SKIP no vdso or no plugin");
        return 0;
    }
    char* address = vdso + (static_cast<char*>(entry) - static_cast<char*>(plugin_info.dli_fbase));
    Dl_info vdso_info {};
    if (dladdr(address, &vdso_info) == 0 || vdso_info.dli_fbase != vdso) {
        std::puts("VDSO_SKIP the vdso is smaller than the plugin's offset");
        return 0;
    }
    print_resolution("vdso", address, address + 1);
    unlink("linux-vdso.so.1");
    rmdir(dir);
    std::puts("DAMAGED_DONE");
    return 0;
}

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "";
    if (std::strcmp(mode, "vdso") == 0) {
        return resolve_vdso_with_planted_file();
    }
    if (std::strcmp(mode, "truncated-alt") == 0 || std::strcmp(mode, "fifo-debuglink") == 0
        || std::strcmp(mode, "fifo-alt") == 0) {
        return load_debuglink_plugin(mode);
    }
    if (std::strcmp(mode, "nomodule") == 0) {
        print_resolution("tiny", reinterpret_cast<void*>(0x1), reinterpret_cast<void*>(0x2));
        void* heap = std::malloc(64);
        print_resolution("heap", heap, static_cast<char*>(heap) + 8);
        std::free(heap);
        std::puts("DAMAGED_DONE");
        return 0;
    }
    const bool truncated = std::strcmp(mode, "truncated") == 0;
    const bool bad_dwarf = std::strcmp(mode, "dwarf") == 0;
    const bool planted = std::strcmp(mode, "planted") == 0;
    const bool bad_index = std::strcmp(mode, "shstrndx") == 0;
    const bool bad_note = std::strcmp(mode, "buildid") == 0;
    const bool deleted = std::strcmp(mode, "deleted") == 0;
    if (!truncated && !bad_dwarf && !planted && !bad_index && !bad_note && !deleted) {
        std::fprintf(stderr,
                     "usage: %s deleted|planted|truncated|dwarf|shstrndx|buildid|vdso|nomodule"
                     "|truncated-alt|fifo-debuglink|fifo-alt\n",
                     argv[0]);
        return 2;
    }
    char dir[64];
    char copy[96];
    if (!copy_plugin(dir, sizeof dir, copy, sizeof copy)) {
        return 2;
    }
    if (truncated) {
        const off_t keep = last_load_end(copy) + 64;
        if (truncate(copy, keep) != 0) {
            std::perror("truncate");
            return 2;
        }
        std::printf("truncated to %ld bytes\n", static_cast<long>(keep));
    }
    if (bad_dwarf && !break_dwarf_version(copy)) {
        std::fprintf(stderr, "no .debug_info in %s\n", copy);
        return 2;
    }
    if (bad_index && !break_section_name_index(copy)) {
        std::perror("shstrndx");
        return 2;
    }
    if (bad_note && !inflate_build_id_note(copy)) {
        std::puts("BUILDID_SKIP the plugin has no .note.gnu.build-id section");
        unlink(copy);
        rmdir(dir);
        return 0;
    }
    void* handle = dlopen(copy, RTLD_NOW);
    if (handle == nullptr) {
        std::fprintf(stderr, "dlopen: %s\n", dlerror());
        return 2;
    }
    if (!truncated && !bad_dwarf && !bad_index && !bad_note) {
        unlink(copy);
    }
    char planted_copy[128];
    std::snprintf(planted_copy, sizeof planted_copy, "%s (deleted)", copy);
    if (planted && !copy_file(DAMAGED_PLUGIN_PATH, planted_copy)) {
        std::perror("plant");
        return 2;
    }
    const int rc = call_plugin(handle);
    unlink(copy);
    unlink(planted_copy);
    rmdir(dir);
    std::puts("DAMAGED_DONE");
    return rc;
}

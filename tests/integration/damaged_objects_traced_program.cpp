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
 */

#include "callStack.h"

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

// Copies the plugin into a fresh temporary directory; fills `dir` and `copy`.
bool copy_plugin(char* dir, size_t dir_size, char* copy, size_t copy_size) {
    std::snprintf(dir, dir_size, "/tmp/cslg_damaged_XXXXXX");
    if (mkdtemp(dir) == nullptr) {
        std::perror("mkdtemp");
        return false;
    }
    std::snprintf(copy, copy_size, "%s/plugin.so", dir);
    FILE* in = std::fopen(DAMAGED_PLUGIN_PATH, "rb");
    FILE* out = std::fopen(copy, "wb");
    if (in == nullptr || out == nullptr) {
        std::perror("fopen");
        return false;
    }
    char buffer[65536];
    size_t got;
    while ((got = std::fread(buffer, 1, sizeof buffer, in)) > 0) {
        if (std::fwrite(buffer, 1, got, out) != got) {
            std::perror("fwrite");
            return false;
        }
    }
    std::fclose(in);
    std::fclose(out);
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

// Sets the version of the first unit in .debug_info to 99. Returns false when
// the file has no .debug_info section.
bool break_dwarf_version(const char* path) {
    FILE* f = std::fopen(path, "r+b");
    if (f == nullptr) {
        return false;
    }
    ElfW(Ehdr) ehdr {};
    bool done = false;
    if (std::fread(&ehdr, sizeof ehdr, 1, f) == 1 && ehdr.e_shoff != 0 && ehdr.e_shstrndx < ehdr.e_shnum) {
        ElfW(Shdr) strings {};
        std::fseek(f, static_cast<long>(ehdr.e_shoff + ehdr.e_shstrndx * sizeof(ElfW(Shdr))), SEEK_SET);
        if (std::fread(&strings, sizeof strings, 1, f) == 1) {
            for (unsigned i = 0; i < ehdr.e_shnum && !done; ++i) {
                ElfW(Shdr) shdr {};
                std::fseek(f, static_cast<long>(ehdr.e_shoff + i * sizeof shdr), SEEK_SET);
                if (std::fread(&shdr, sizeof shdr, 1, f) != 1) {
                    break;
                }
                char name[16] = {};
                std::fseek(f, static_cast<long>(strings.sh_offset + shdr.sh_name), SEEK_SET);
                if (std::fread(name, 1, sizeof name - 1, f) == 0 || std::strcmp(name, ".debug_info") != 0) {
                    continue;
                }
                // 32-bit DWARF unit header: unit_length (4 bytes), then version (2).
                const unsigned char version[2] = { 99, 0 };
                std::fseek(f, static_cast<long>(shdr.sh_offset + 4), SEEK_SET);
                done = std::fwrite(version, 1, sizeof version, f) == sizeof version;
            }
        }
    }
    std::fclose(f);
    return done;
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

} // namespace

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
    if (!truncated && !bad_dwarf && !planted && std::strcmp(mode, "deleted") != 0) {
        std::fprintf(stderr, "usage: %s deleted|planted|truncated|dwarf|vdso|nomodule\n", argv[0]);
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
    void* handle = dlopen(copy, RTLD_NOW);
    if (handle == nullptr) {
        std::fprintf(stderr, "dlopen: %s\n", dlerror());
        return 2;
    }
    if (!truncated && !bad_dwarf) {
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

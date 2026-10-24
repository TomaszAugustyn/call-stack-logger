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
 */

#include "callStack.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h> // for ElfW
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

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "";
    if (std::strcmp(mode, "nomodule") == 0) {
        print_resolution("tiny", reinterpret_cast<void*>(0x1), reinterpret_cast<void*>(0x2));
        void* heap = std::malloc(64);
        print_resolution("heap", heap, static_cast<char*>(heap) + 8);
        std::free(heap);
        std::puts("DAMAGED_DONE");
        return 0;
    }
    const bool truncated = std::strcmp(mode, "truncated") == 0;
    if (!truncated && std::strcmp(mode, "deleted") != 0) {
        std::fprintf(stderr, "usage: %s deleted|truncated|nomodule\n", argv[0]);
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
    void* handle = dlopen(copy, RTLD_NOW);
    if (handle == nullptr) {
        std::fprintf(stderr, "dlopen: %s\n", dlerror());
        return 2;
    }
    if (!truncated) {
        unlink(copy);
    }
    const int rc = call_plugin(handle);
    unlink(copy);
    rmdir(dir);
    std::puts("DAMAGED_DONE");
    return rc;
}

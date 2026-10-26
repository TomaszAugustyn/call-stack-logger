/*
 * Copyright © 2020-2026 Tomasz Augustyn
 * All rights reserved.
 *
 * Project Name: Call Stack Logger
 * GitHub: https://github.com/TomaszAugustyn/call-stack-logger
 * Contact Email: t.augustyn@poczta.fm
 */

/*
 * Instrumented driver for DlopenReloadTest. Loads reload_lib_a, calls into it
 * so that the resolver registers it, unloads it, loads the smaller
 * reload_lib_b and calls into that. The test is about b's code landing on
 * addresses that a's code occupied, at a different base: a resolver that kept
 * a's module around would answer with a's names there.
 *
 * The kernel places a library at the top of the highest free gap that fits
 * it, and the top of a's old range holds its unwind tables and data, not its
 * code. So after unloading a, the driver reserves that non-code tail with a
 * temporary PROT_NONE mapping; the top of the remaining hole is then the end
 * of a's old code segment, and b lands inside it. Prints RELOAD_OVERLAP=1 when
 * b's entry point does lie inside a's old executable segment, 0 otherwise (the
 * test then skips). C-style I/O only: this translation unit is instrumented
 * (see damaged_objects_traced_program.cpp for why).
 */

#include <cstdint>
#include <cstdio>
#include <dlfcn.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <unistd.h>

namespace {

// The gaps the kernel finds depend on the address-space randomization; the
// placement above is reliable only without it. So the driver re-executes
// itself once without randomization, before main() and before any hook.
// Where the personality call is refused (some container seccomp profiles), it
// runs randomized and the test may skip.
__attribute__((constructor, no_instrument_function)) void run_without_address_randomization() {
    const int current = personality(0xffffffff);
    if (current == -1 || (current & ADDR_NO_RANDOMIZE) != 0) {
        return;
    }
    if (personality(static_cast<unsigned long>(current) | ADDR_NO_RANDOMIZE) == -1) {
        return;
    }
    execl("/proc/self/exe", "/proc/self/exe", static_cast<char*>(nullptr));
    // Only reached when exec failed: carry on randomized.
}

struct Layout {
    std::uintptr_t bias = 0;
    std::uintptr_t code_low = UINTPTR_MAX;  // the executable segment
    std::uintptr_t code_high = 0;
    std::uintptr_t high = 0;                // the end of the whole object
};

// The executable segment and the end of the loaded object whose load bias is
// `layout->bias`.
int collect_layout(dl_phdr_info* info, size_t, void* data) {
    auto* layout = static_cast<Layout*>(data);
    if (info->dlpi_addr != layout->bias) {
        return 0;
    }
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& phdr = info->dlpi_phdr[i];
        if (phdr.p_type != PT_LOAD) {
            continue;
        }
        const std::uintptr_t start = info->dlpi_addr + phdr.p_vaddr;
        const std::uintptr_t end = start + phdr.p_memsz;
        if ((phdr.p_flags & PF_X) != 0) {
            layout->code_low = start;
            layout->code_high = end;
        }
        if (end > layout->high) {
            layout->high = end;
        }
    }
    return 1;
}

std::uintptr_t load_bias_of(void* address) {
    Dl_info info {};
    link_map* map = nullptr;
    if (dladdr1(address, &info, reinterpret_cast<void**>(&map), RTLD_DL_LINKMAP) == 0 || map == nullptr) {
        return 0;
    }
    return map->l_addr;
}

std::uintptr_t page_up(std::uintptr_t address) {
    const std::uintptr_t page = static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
    return (address + page - 1) & ~(page - 1);
}

} // namespace

int main() {
    void* a = dlopen(RELOAD_LIB_A_PATH, RTLD_NOW);
    if (a == nullptr) {
        std::fprintf(stderr, "dlopen a: %s\n", dlerror());
        return 2;
    }
    auto a_entry = reinterpret_cast<int (*)(int)>(dlsym(a, "reload_a_entry"));
    Layout layout;
    layout.bias = load_bias_of(reinterpret_cast<void*>(a_entry));
    dl_iterate_phdr(collect_layout, &layout);
    std::printf("a result=%d\n", a_entry(1));
    dlclose(a);

    // Reserve a's old non-code tail, so that b lands at the end of a's old code.
    void* tail = MAP_FAILED;
    const std::uintptr_t tail_start = page_up(layout.code_high);
    if (layout.high > tail_start) {
        tail = mmap(reinterpret_cast<void*>(tail_start), layout.high - tail_start, PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    }
    void* b = dlopen(RELOAD_LIB_B_PATH, RTLD_NOW);
    if (tail != MAP_FAILED) {
        munmap(tail, layout.high - tail_start);
    }
    if (b == nullptr) {
        std::fprintf(stderr, "dlopen b: %s\n", dlerror());
        return 2;
    }
    auto b_entry = reinterpret_cast<int (*)(int)>(dlsym(b, "reload_b_entry"));
    const std::uintptr_t b_address = reinterpret_cast<std::uintptr_t>(b_entry);
    const bool overlap = b_address >= layout.code_low && b_address < layout.code_high
                         && load_bias_of(reinterpret_cast<void*>(b_entry)) != layout.bias;
    std::printf("RELOAD_OVERLAP=%d\n", overlap ? 1 : 0);
    std::printf("b result=%d\n", b_entry(1));
    std::puts("RELOAD_DONE");
    return 0;
}

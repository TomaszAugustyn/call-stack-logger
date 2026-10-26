# Third-party components

This file lists every component outside Call Stack Logger's own code that ends
up in a program built with `callstacklogger::instrumented`, with its license,
how it is linked and what the license asks of someone who distributes the
program. It is the file the commercial license's third-party clause refers to.
The classification follows the licenses' own texts and the usual industry
reading of them; it is not legal advice, and a lawyer should confirm it before
the first commercial sale.

## Components

Since v1.0.0, addresses are resolved with libbacktrace, which is licensed under
a 3-clause BSD license. It is compiled from source into the library, and the
only other components a program needs are the C and C++ runtime libraries of
the system, which carry permissive terms, weak-copyleft terms that dynamic
linking satisfies, or the GCC Runtime Library Exception. A closed-source
product can therefore ship an instrumented diagnostic build under the
commercial license, with the notices listed below and nothing else. Building
the library needs no binutils, zlib or zstd development package.

| Component | Used for | License | How it is linked | Notice obligation |
| --- | --- | --- | --- | --- |
| Call Stack Logger | the tracer itself | AGPL-3.0 or a commercial license | static library `libcallstacklogger.a` | AGPL: source of the whole program; commercial: per the agreement |
| libbacktrace (written by Ian Lance Taylor, copyright Free Software Foundation, Inc.; commit `0b9b49cf4a2c9229fc052d6716e1528b2f23e91a` of 3 September 2026) | reading the symbol tables and the DWARF debug information of the loaded object files: function names, file and line, inline chains, separate debug files, zlib/zstd-compressed debug sections | BSD-3-Clause (**permissive**) | compiled from source into `libcallstacklogger.a`, symbols hidden and renamed (`cslg_vendored_backtrace_*`); only the C sources, the headers and `LICENSE` of the repository are used — its GPL-licensed autotools helper files (`configure`, `config.guess`, `ltmain.sh`, ...) are not | keep the copyright notice, the license conditions and the disclaimer in the documentation or other materials that come with a binary distribution (the text is at the end of this file and in `third_party/libbacktrace/LICENSE`); do not use the author's name to endorse a product |
| glibc: `libc.so.6`, `libm.so.6`, the dynamic loader (`libdl` and `libpthread` are part of `libc` since glibc 2.34) | C library, `dladdr1`, threads | LGPL-2.1-or-later with smaller parts under BSD and other permissive terms (**weak copyleft**, and a system library) | dynamic (`NEEDED`) | none in practice: the LGPL's requirement that the user can relink against a modified library is met by dynamic linking; nothing of glibc is copied into the program |
| libstdc++ (`libstdc++.so.6`) | C++ standard library, `__cxa_demangle`, the exception runtime | GPL-3.0-or-later **with the GCC Runtime Library Exception, version 3.1** | dynamic (`NEEDED`) | none: the exception grants permission to distribute programs compiled with GCC or Clang that use the runtime library without GPL obligations on the program. A reader who sees "GPL" next to libstdc++ need not worry — this is the same situation as for every C++ program on Linux |
| libgcc (`libgcc_s.so.1`) | the compiler's runtime support, stack unwinding | GPL-3.0-or-later **with the GCC Runtime Library Exception, version 3.1** | dynamic (`NEEDED`) | none, as for libstdc++ |
| Google Test 1.14.0 | the test suite (`BUILD_TESTS=ON`, top-level builds only) | BSD-3-Clause (**permissive**) | downloaded at configure time, linked into the test executables only; not part of the library or of any user program | none for shipped programs; the notice applies only to a distribution of the test executables |

Headers included by the library's own translation units from outside the C++
standard library and glibc: `backtrace.h` of libbacktrace (private; the public
header `include/callStack.h` includes no third-party header).

## Licensing history: releases before v1.0.0 used libbfd

Releases before v1.0.0 resolved addresses with libbfd, the "Binary File
Descriptor" library of GNU binutils, which is licensed under the GNU GPL,
version 3 or later. On Ubuntu it is a shared library (`libbfd-2.42-system.so`
on 24.04) that every instrumented program listed as `NEEDED`; on Fedora it is a
static archive, and 57 of its object files, 25 of libiberty's and one of
libsframe's were copied into every instrumented program. Either way a shipped
program contained GPL code, so the whole program had to be offered under the
GPL, and a commercial license for Call Stack Logger — which can only cover Call
Stack Logger's own code — could not change that. Using those releases in-house,
in debug builds that are never distributed, never created an obligation;
distribution did.

What a program built with the last of those releases contained, measured with a
sample program built with `callstacklogger::instrumented`, `LOG_ELAPSED=ON` and
`LOG_EXCEPTIONS=ON`, in Release and Debug (the link sets were the same in both),
on 24 October 2026:

- Fedora 44 (GCC 16.2.1, binutils-devel 2.46.1, glibc 2.43):
  `readelf -d` listed `libz.so.1`, `libzstd.so.1`, `libstdc++.so.6`,
  `libm.so.6`, `libgcc_s.so.1`, `libc.so.6`; the link map showed 57 members of
  `libbfd.a`, 25 members of `libiberty.a` and 1 member of `libsframe.a` copied
  into the executable (9.0 MB in total).
- Ubuntu 24.04 (GCC 13.3, binutils-dev 2.42, glibc 2.39): `readelf -d` listed
  `libbfd-2.42-system.so`, `libstdc++.so.6`, `libgcc_s.so.1`, `libc.so.6`;
  `ldd` added, through libbfd, `libz.so.1`, `libzstd.so.1` and
  `libsframe.so.1`.

| Component | Used for | License | How it was linked | Consequence |
| --- | --- | --- | --- | --- |
| libbfd (GNU binutils) | symbol and line resolution | GPL-3.0-or-later (**strong copyleft**) | Ubuntu: dynamic, `libbfd-<version>-system.so` from package `libbinutils`; Fedora: static, `libbfd.a` copied into the program | the distributed program had to be offered under the GPL; blocked closed-source distribution |
| libiberty (part of binutils, inside libbfd) | demangling, hash tables, memory helpers used by libbfd | a mix: of the 25 object files Fedora linked, 7 are GPL-2.0-or-later (`cp-demangle.c`, `filename_cmp.c`, `lrealpath.c`, `make-relative-prefix.c`, `objalloc.c`, `splay-tree.c`, `unlink-if-ordinary.c`), 15 are LGPL-2.0/2.1-or-later and 3 are public domain (`xmemdup.c`, `xstrdup.c`, `xstrerror.c`); the README of libiberty applies the LGPL to files without a notice (`getpwd.c`) | Ubuntu: compiled into `libbfd-<version>-system.so`; Fedora: static, `libiberty.a` | GPL files inside the program, same consequence as libbfd |
| libsframe (binutils) | SFrame stack-trace format, pulled in by libbfd | GPL-3.0-or-later (**strong copyleft**) | Ubuntu: dynamic `libsframe.so.1` via libbfd; Fedora: static `libsframe.a` | same consequence as libbfd |
| zlib (Ubuntu `zlib1g`; Fedora `zlib-ng-compat`) | zlib-compressed debug sections, for libbfd | Zlib license (**permissive**) | Ubuntu: dynamic via libbfd; Fedora: dynamic, `libz.so.1` listed by the program itself | notice only |
| zstd (`libzstd1` / `libzstd`) | zstd-compressed debug sections, for libbfd | BSD-3-Clause or GPL-2.0-only, at the distributor's choice (**permissive** under the BSD option) | Ubuntu: dynamic via libbfd; Fedora: dynamic, `libzstd.so.1` listed by the program itself (and `libzstd-devel` needed to link) | notice only (BSD option) |
| binutils headers `bfd.h`, `ansidecl.h`, `symcat.h`, `diagnostics.h` | included by the library's translation units and, through the public header `callStack.h`, by every consumer translation unit that included it | GPL-3.0-or-later | compile time | the public header required `binutils-dev` on every consumer's machine |
| glibc, libstdc++, libgcc, Google Test | as above | as above | as above | as above |

libbfd was the only strong-copyleft dependency of those releases; libiberty,
libsframe and the two compression libraries came with it.

## Sources of the license statements

- Fedora: `rpm -q --qf '%{LICENSE}' binutils-devel zlib-ng-compat libzstd glibc libstdc++ libgcc` and the files under `/usr/share/licenses/` (`binutils/COPYING3`, `libgcc/COPYING.RUNTIME`, `libzstd/LICENSE`, `glibc/COPYING.LIB`).
- Ubuntu 24.04: `/usr/share/doc/binutils-common/copyright` ("GNU General Public License ... either version 3"), `/usr/share/doc/gcc-14-base/copyright` (section on the GCC Runtime Library Exception), `/usr/share/doc/libc6/copyright`, `/usr/share/doc/zlib1g/copyright`, `/usr/share/doc/libzstd1/copyright`.
- libiberty: the license header of each source file in the binutils-gdb repository (`libiberty/*.c`, checked on 24 October 2026).
- libbacktrace: the `LICENSE` file of the repository at the pinned commit, reproduced below.

## libbacktrace license text

```
Copyright (C) 2012-2016 Free Software Foundation, Inc.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

    (1) Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

    (2) Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in
    the documentation and/or other materials provided with the
    distribution.

    (3) The name of the author may not be used to
    endorse or promote products derived from this software without
    specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
```

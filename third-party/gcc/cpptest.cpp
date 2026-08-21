// cpptest.cpp — minimal C++-runtime probe (bead vms-5562, epic vms-da0 F2b).
//
// GOAL: surface the C++-runtime walls for linking a C++ TU as an OVMX image
// via LINK.EXE, WITHOUT attempting the full 300MB cc1/cc1plus link (that is
// the next rung once these walls are known — see host-probe-cc1.sh / F2a for
// why cc1/cc1plus itself was built HOSTED, not as an OVMX image). This TU
// deliberately exercises exactly the three novel C++-runtime facilities the
// design doc predicts as F2 forcing functions:
//
//   1. A std::string operation (libstdc++ heap-allocating container: pulls
//      operator new/delete, __cxa_atexit-registered static teardown for any
//      global-scope instance, and the small-string-optimization/allocator
//      path).
//   2. A try{}catch(std::exception&) block that actually throws (exercises
//      __cxa_throw/__cxa_begin_catch/__cxa_end_catch, the Itanium C++ ABI
//      personality routine __gxx_personality_v0, and the DWARF unwinder's
//      table lookup — .eh_frame/.gcc_except_table, the exact section pair
//      the design doc calls out).
//   3. A global object with a NON-TRIVIAL constructor (forces a real
//      .init_array entry that calls INTO libstdc++/libsupc++, not just a
//      POD initializer — the same forcing function bfd.c's page-size cache
//      ctor was for AS.EXE in vms-0b6b/vms-ee2, one level deeper: this ctor
//      calls a C++ runtime function, not just a musl libc one).
//
// Compiled HOSTED (see mk_cpptest_ovmx.sh — NOT -ffreestanding: C++ standard
// headers #error under a freestanding toolchain, same reason host-probe-
// cc1.sh drops -ffreestanding for cc1/cc1plus itself). Only LINK.EXE's
// consumption of the resulting hosted .o + the whole-archived upstream
// libstdc++.a/libsupc++.a/libgcc.a/libgcc_eh.a, and IMGACT.EXE's activation
// of the result, are OVMX-native here.
//
// Clean-room (Rule 8): plain ISO C++ + libstdc++ (GPL), no VSI/HPE.

#include <cstdio>
#include <string>
#include <stdexcept>

// (3) Global object, non-trivial constructor -- forces .init_array to carry
// a REAL entry point that calls into libstdc++ (std::string's constructor),
// not a POD zero-fill. Runs before main() via IMGACT's constructor pass
// (vms-ee2) if that pass reaches this image's .init_array at all.
struct Greeter {
    std::string tag;
    Greeter() : tag("cpptest-ctor-ran") {
        std::fputs("[Greeter::Greeter] global ctor executed\n", stdout);
    }
} g_greeter;

int main() {
    // (1) std::string op -- heap alloc via operator new, SSO buffer, and
    // (at scope exit / atexit time) the destructor path.
    std::string s = "OVMX C++ probe: ";
    s += "hello from cpptest";
    std::printf("%s\n", s.c_str());
    std::printf("global ctor tag = %s\n", g_greeter.tag.c_str());

    // (2) try/catch across a real throw -- exercises __cxa_throw, the
    // Itanium personality routine, and the unwinder's .eh_frame/
    // .gcc_except_table lookup.
    try {
        throw std::runtime_error("cpptest exception");
    } catch (const std::exception &e) {
        std::printf("caught: %s\n", e.what());
    }

    std::printf("cpptest: OK\n");
    return 0;
}

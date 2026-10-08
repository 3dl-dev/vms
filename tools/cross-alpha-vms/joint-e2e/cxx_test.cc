// C++ on OVMX/Alpha (vms-4d0): built by the stage-2 alpha-dec-vms g++ against
// the OVMX C RTL, linked by OVMX LINK.EXE with libstdc++/libgcc pulled by
// library search, activated on the real executive. Exercises:
//   - a static constructor (crtbegin/crtend ctors list, run via LIB$INITIALIZE),
//   - virtual dispatch + new/delete (libsupc++),
//   - std::string / std::vector (libstdc++),
//   - a C++ exception thrown and caught across a frame (libgcc's DWARF unwinder,
//     EH frames registered by crtbegin's frame_dummy).
// Sentinel 7 = all of it held.
#include <cstdio>
#include <string>
#include <vector>
#include <stdexcept>

struct B { virtual int f() const { return 1; } virtual ~B() {} };
struct D : B { int f() const override { return 7; } };

static int ctor_ran = 0;
struct Init { Init() { ctor_ran = 42; } };
static Init g_init;

__attribute__((noinline)) static int thrower(int x)
{
    if (x > 0)
        throw std::runtime_error("ovmx");
    return x;
}

int main(int argc, char **argv)
{
    (void)argv;
    B *b = new D;
    std::vector<std::string> v;
    v.push_back("OVMX");
    v.push_back(std::string("C++") + "/libstdc++");
    int caught = 0;
    try {
        thrower(argc);
    } catch (const std::runtime_error &e) {
        caught = (std::string(e.what()) == "ovmx");
    }
    int virt = b->f();
    std::printf("OVMX C++ test: ctor=%d virt=%d vec=%s,%s caught=%d argc=%d ptr=%u\n",
                ctor_ran, virt, v[0].c_str(), v[1].c_str(), caught, argc,
                (unsigned)(sizeof(void *) * 8));
    int ok = ctor_ran == 42 && virt == 7 && v[1] == "C++/libstdc++" && caught;
    delete b;
    return ok ? 7 : 3;
}

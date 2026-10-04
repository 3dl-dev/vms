#include <cstdio>
#include <string>
#include <vector>
struct B { virtual int f() const { return 1; } virtual ~B() {} };
struct D : B { int f() const override { return 7; } };
static int ctor_ran = 0;
struct Init { Init() { ctor_ran = 1; } } g_init;
int main()
{
    B *b = new D;
    std::vector<std::string> v;
    v.push_back("OVMX");
    v.push_back(std::string("C++") + "/libstdc++");
    std::printf("OVMX C++ test: %s %s virt=%d ctor=%d\n", v[0].c_str(), v[1].c_str(), b->f(), ctor_ran);
    int r = b->f();
    delete b;
    return r;
}

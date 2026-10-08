// cxx_test_tu2.cc -- the second translation unit of the C++ gate program: it
// returns the address of the inline variable as THIS unit sees it.
#include "cxx_test_iv.h"
int *ovmx_iv_tu2() { return ovmx_iv; }

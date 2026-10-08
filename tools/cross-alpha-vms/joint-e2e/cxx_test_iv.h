// cxx_test_iv.h -- a C++17 inline variable shared by cxx_test.cc and
// cxx_test_tu2.cc (vms-fd1, GCC patch 0013). Both translation units define it;
// the program must link and hold ONE object: the address each unit sees is the
// same. Unpatched, alpha-dec-vms emitted it as a strong global in both objects
// and LINK refused the pair (MULDEF), as it refused cc1's tree_code_type.
#ifndef CXX_TEST_IV_H
#define CXX_TEST_IV_H
inline int ovmx_iv[] = { 11, 22, 33 };
int *ovmx_iv_tu2();
#endif

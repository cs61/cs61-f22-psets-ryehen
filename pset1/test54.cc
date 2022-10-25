#include "m61.hh"
#include <cstdio>
#include <cassert>
#include <cstring>
// Ensure realloc returns nullptr when called on ptr not previously allocated
int main() {
    void* oldPtr = 0;
    void* newPtr =  m61_realloc(oldPtr, 1);
    assert(newPtr == nullptr);
    m61_print_statistics();
}

//! alloc count: active          0   total          0   fail          1
//! alloc size:  active          0   total          0   fail          1

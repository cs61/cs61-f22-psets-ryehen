#include "m61.hh"
#include <cstdio>
#include <cassert>
#include <cstring>
// Ensure realloc returns nullptr when requested size is 0
int main() {
    int* oldPtr = (int*) m61_malloc(sizeof(int));
    void* newPtr =  m61_realloc((void*) oldPtr, 0);
    assert(newPtr == nullptr);
    m61_print_statistics();
}

//! alloc count: active          1   total          1   fail          1
//! alloc size:  active          4   total          4   fail          0

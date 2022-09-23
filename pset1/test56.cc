#include "m61.hh"
#include <cstdio>
#include <cassert>
#include <cstring>
// Ensure realloc performs enlargement reallocation
int main() {
    int* oldPtr = (int*) m61_malloc(sizeof(int));
    m61_print_statistics();
    
    double* newPtr =  (double*) m61_realloc((void*) oldPtr, sizeof(double));
    assert(newPtr != nullptr);
    m61_print_statistics();
}

//! alloc count: active          1   total          1   fail          0
//! alloc size:  active          4   total          4   fail          0
//! alloc count: active          1   total          2   fail          0
//! alloc size:  active          8   total         12   fail          0

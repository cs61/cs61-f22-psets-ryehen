#include "m61.hh"
#include <cstdio>
#include <cassert>
#include <cstring>
// Ensure realloc performs downsizing reallocation
int main() {
    double* oldPtr = (double*) m61_malloc(sizeof(double));
    m61_print_statistics();
    
    int* newPtr = (int*) m61_realloc((void*) oldPtr, sizeof(int));
    assert(newPtr != nullptr);
    m61_print_statistics();
}


//! alloc count: active          1   total          1   fail          0
//! alloc size:  active          8   total          8   fail          0
//! alloc count: active          1   total          2   fail          0
//! alloc size:  active          4   total         12   fail          0

#include "m61.hh"
#include <cstdio>
#include <cassert>
#include <cstring>
// Check calloc handles requested size of 0

int main() {
    void* p1 = m61_calloc(0, 0);
    assert(p1 == nullptr);

    void* p2 = m61_calloc(0, 100);
    assert(p2 == nullptr);

    void* p3 = m61_calloc(100, 0);
    assert(p3 == nullptr);
    m61_print_statistics();
}

//! alloc count: active          0   total          0   fail          3
//! alloc size:  active          0   total          0   fail        ???

#include "m61.hh"
#include <cstdio>
#include <cassert>
#include <cstring>

// Ensures boundary writes that write past, but not directly on, the boundary are caught
int main() {
    int* ptr = (int*) m61_malloc(sizeof(int) * 9);
    fprintf(stderr, "Will free %p\n", ptr);

    // Will end up writing past boundary, but not directly ON boundary
    for (int i = 0; i <= 10; ++i) {
        if (i % 2 == 0) {
            ptr[i] = i;
        }
    }
    m61_free(ptr);
    m61_print_statistics();
}

//! Will free ??{0x\w+}=ptr??
//! MEMORY BUG???: detected wild write during free of pointer ??ptr??
//! ???

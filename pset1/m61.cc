#include "m61.hh"
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cinttypes>
#include <cassert>
#include <sys/mman.h>
#include <unordered_map>
#include <map>
#include <iostream>

// Stores info regarding each block of memory
struct metadata {
    const char* file;
    int line;
    size_t size;
    size_t pos;
    bool freed;
};

// List of pointers that have been freed
// Reduces time
std::vector<void*> freedPointers;

// Map from ptr to region size
std::map<void*, size_t> freeRegions;
// Maps ptr address to metadata
std::map<void*, metadata> metadataMap;

struct m61_memory_buffer {
    char* buffer;
    size_t pos = 0;
    size_t size = 8 << 20; /* 8 MiB */

    m61_memory_buffer();
    ~m61_memory_buffer();
};

static m61_memory_buffer default_buffer;


m61_memory_buffer::m61_memory_buffer() {
    void* buf = mmap(nullptr,    // Place the buffer at a random address
        this->size,              // Buffer should be 8 MiB big
        PROT_WRITE,              // We want to read and write the buffer
        MAP_ANON | MAP_PRIVATE, -1, 0);
                                 // We want memory freshly allocated by the OS
    assert(buf != MAP_FAILED);
    this->buffer = (char*) buf;
}

m61_memory_buffer::~m61_memory_buffer() {
    munmap(this->buffer, this->size);
}

unsigned long long nactive = 0;           // number of active allocations [#malloc - #free]
unsigned long long active_size = 0;       // number of bytes in active allocations
unsigned long long ntotal = 0;            // number of allocations, total
unsigned long long total_size = 0;        // number of bytes in allocations, total
unsigned long long nfail = 0;             // number of failed allocation attempts
unsigned long long fail_size = 0;         // number of bytes in failed allocation attempts
uintptr_t heap_min;                       // smallest address in any region ever allocated
uintptr_t heap_max;                       // largest address in any region ever allocated


/// m61_malloc(sz, file, line)
///    Returns a pointer to `sz` bytes of freshly-allocated dynamic memory.
///    The memory is not initialized. If `sz == 0`, then m61_malloc may
///    return either `nullptr` or a pointer to a unique allocation.
///    The allocation request was made at source code location `file`:`line`.

void* m61_malloc(size_t sz, const char* file, int line) {
    (void) file, (void) line;   // avoid uninitialized variable warnings

    void* ptr = nullptr;
    if (nactive == 0) {
        default_buffer.pos = 0;
        freeRegions.clear();
        freeRegions[&default_buffer.buffer[0]] = 8 << 20;
    }
    size_t currentPos = default_buffer.pos;
    unsigned long alignmentDiff = (uintptr_t) sz % alignof(std::max_align_t);
    if (currentPos + sz > default_buffer.size || currentPos + sz < sz) {
        // Not enough space left in default buffer for allocation
        bool sufficientBlockFound = false;
        
        // Check if space can be found among freed pointers
        if (freeRegions.size() > 0) {
            for (auto& [regionPtr, regionSize] : freeRegions) {
                if (regionSize >= sz) {
                    ptr = regionPtr;
                    sufficientBlockFound = true;
                    if (regionSize - sz > 0 ) {
                        freeRegions[(void*) ((uintptr_t) regionPtr + sz + alignmentDiff)] = regionSize - sz - alignmentDiff;
                    }
                    freeRegions.erase(regionPtr);
                    break;           
                }
            }
        }

        if (!sufficientBlockFound) {
            nfail++;
            fail_size += sz;
            return ptr;
        }
    }   

    // Otherwise there is enough space; claim the next `sz` bytes
    if (ptr == nullptr) {
        ptr = &default_buffer.buffer[currentPos]; 
    }
    
    
    // Store ptr metadata
    metadataMap[ptr] = {file, line, sz, currentPos, false};

    // Update max / min heap
    if (!heap_min || (uintptr_t) ptr < heap_min) {
        heap_min = (uintptr_t) ptr;
    }

    if (!heap_max || (uintptr_t) ptr + sz > heap_max) {
        heap_max = (uintptr_t) ptr + sz;
    }

    // Only increment buffer pos if we're not using realloced memory
    if (currentPos == default_buffer.pos) {
        // Preserve alignment as we update buffer pos
        default_buffer.pos += (sz + alignmentDiff);
    }
    
    ntotal++;
    nactive++;
    active_size += sz;
    total_size += sz;

    return ptr;
}


/// m61_free(ptr, file, line)
///    Frees the memory allocation pointed to by `ptr`. If `ptr == nullptr`,
///    does nothing. Otherwise, `ptr` must point to a currently active
///    allocation returned by `m61_malloc`. The free was called at location
///    `file`:`line`.

void m61_free(void* ptr, const char* file, int line) {
    // avoid uninitialized variable warnings
    (void) ptr, (void) file, (void) line;
    // Your code here. The handout code does nothing!
    if (ptr == nullptr) {
        return;
    }

    const char* errmsg = "";
    if (metadataMap.count(ptr) > 0) {
        --nactive;
        active_size -= metadataMap[ptr].size;
        metadataMap[ptr].freed = true;

        // Update info on contiguous regions w space for allocation
        bool contiguousFound = false;
        size_t sz = metadataMap[ptr].size;
        size_t alignmentAdjustment = sz % alignof(std::max_align_t);
        
        void* adjacentAddress = (void*)((uintptr_t) ptr + sz + alignmentAdjustment);
        if (freeRegions.count(adjacentAddress) == 1) {
            freeRegions[ptr] = freeRegions[adjacentAddress] + sz + alignmentAdjustment;
            freeRegions.erase(adjacentAddress);
            contiguousFound = true;
        }
        
        for (auto& [regionPtr, size] : freeRegions) {
            if ((void*) ((uintptr_t) regionPtr + size) == ptr) {
                if (contiguousFound) {
                    freeRegions[regionPtr] += freeRegions[ptr];
                    freeRegions.erase(ptr);
                } else {
                    freeRegions[regionPtr] += (sz + alignmentAdjustment);  
                }
                contiguousFound = true;
                break;
            }
        }

        if (!contiguousFound) {
            freeRegions[ptr] = sz + alignmentAdjustment;
        }

        // for (auto& [whatever, size] : freeRegions) {
        //     std::cout << size << std::endl;
        // }
        // m61_print_statistics();
        // std::cout << freeRegions.size() << std::endl;
        
    }
    return;
}


/// m61_calloc(count, sz, file, line)
///    Returns a pointer a fresh dynamic memory allocation big enough to
///    hold an array of `count` elements of `sz` bytes each. Returned
///    memory is initialized to zero. The allocation request was at
///    location `file`:`line`. Returns `nullptr` if out of memory; may
///    also return `nullptr` if `count == 0` or `size == 0`.

void* m61_calloc(size_t count, size_t sz, const char* file, int line) {
    // Your code here (to fix test019).
    if (default_buffer.pos + count * sz > default_buffer.size || ( count != 1 && count * sz <= sz)) {
        nfail++;
        return nullptr;
    }

    void* ptr = m61_malloc(count * sz, file, line);
    if (ptr) {
        memset(ptr, 0, count * sz);
    }
    return ptr;
}


/// m61_get_statistics()
///    Return the current memory statistics.

m61_statistics m61_get_statistics() {
    // Your code here.
    // The handout code sets all statistics to enormous numbers.
    m61_statistics stats;
    stats.nactive = nactive;
    stats.active_size = active_size;
    stats.ntotal = ntotal;
    stats.total_size = total_size;
    stats.nfail = nfail;
    stats.fail_size = fail_size;
    stats.heap_min = heap_min;
    stats.heap_max = heap_max;
    //memset(&stats, 0, sizeof(m61_statistics));
    return stats;
}


/// m61_print_statistics()
///    Prints the current memory statistics.

void m61_print_statistics() {
    m61_statistics stats = m61_get_statistics();
    printf("alloc count: active %10llu   total %10llu   fail %10llu\n",
           stats.nactive, stats.ntotal, stats.nfail);
    printf("alloc size:  active %10llu   total %10llu   fail %10llu\n",
           stats.active_size, stats.total_size, stats.fail_size);
}


/// m61_print_leak_report()
///    Prints a report of all currently-active allocated blocks of dynamic
///    memory.

void m61_print_leak_report() {
    // Your code here.
}

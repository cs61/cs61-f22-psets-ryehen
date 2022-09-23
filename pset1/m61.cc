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
#include <unordered_set>
#include "hexdump.hh"


const int MaxAlignment = alignof(std::max_align_t);

// Stores info regarding each block of memory
struct startingMetadata {               // Distances from payload pointer start
    const char* file;                   // -32
    size_t size;                        // -24
    int line;                           // -16 
    bool freed;                         // -12
    char allocationKey;                 // -11
};

// Deliberately of size 8
struct endingMetadata {
    size_t size;
    char boundaryKey[4];
};

// Case in which a region was never allocated
struct deadMetadata {
    char metadata[32];
};

const int startingMetadataAlottment = 32;
const int endingMetadataAlottment = 8;
const int totalMetadataAlottment = startingMetadataAlottment + endingMetadataAlottment;
const char allocationKey = '|';

// Map from ptr to region size
std::map<uintptr_t, size_t> freeRegions;
std::unordered_set<uintptr_t> activePointers;

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

bool firstCall = true;
void* m61_malloc(size_t sz, const char* file, int line) {
    (void) file, (void) line;   // avoid uninitialized variable warnings

    void* ptr = nullptr;
    if (nactive == 0) {
        default_buffer.pos = 0;
        
        if (firstCall) {
            memset(&default_buffer.buffer[0], 'X', default_buffer.size);
            firstCall = false;
        }

        freeRegions.clear();
        activePointers.clear();
        freeRegions[(uintptr_t) &default_buffer.buffer[0]] = 8 << 20;
    }
    size_t currentPos = default_buffer.pos;

    // Calculate alignment adjustment required
    int alignmentRemainder = (sz + endingMetadataAlottment) % MaxAlignment;
    int alignmentAdjustment = (alignmentRemainder == 0) ? 0 : MaxAlignment - alignmentRemainder;
    
    // If integer overflow is occuring, return nullptr
    if (currentPos + sz < sz) {
        nfail++;
        fail_size += sz;
        return nullptr;
    }

    // Allocate to first sufficient free region
    bool sufficientBlockFound = false;
    if (freeRegions.size() > 0) {
        for (auto& [regionPtr, regionSize] : freeRegions) {
            if (regionSize >= sz + totalMetadataAlottment && sz + totalMetadataAlottment > sz) {
                ptr = (void*) regionPtr;
                sufficientBlockFound = true;
                
                int newRegionSize = regionSize - sz - alignmentAdjustment - totalMetadataAlottment;
                activePointers.insert((uintptr_t) regionPtr + startingMetadataAlottment);
                if ( newRegionSize > 0 ) {
                    freeRegions[((uintptr_t) regionPtr + sz + alignmentAdjustment + totalMetadataAlottment)] = newRegionSize;
                }
                freeRegions.erase(regionPtr);
                break;
            }
        }
    }
    
    if (!sufficientBlockFound) {
        nfail++;
        fail_size += sz;
        return nullptr;
    }
    
    // Store ptr metadata internally
    startingMetadata* startingMetadataPtr = (startingMetadata*) ptr;
    *startingMetadataPtr = (startingMetadata) {file, sz, line, false, '|'};
    

    endingMetadata* endingMetadataPtr = (endingMetadata*) ((uintptr_t) ptr + sz + startingMetadataAlottment);
    *endingMetadataPtr = (endingMetadata) {sz, {'|', '|', '|', '|'}};

    // Update max / min heap
    if (!heap_min || (uintptr_t) ptr < heap_min) {
        heap_min = (uintptr_t) ptr;
    }

    if (!heap_max || (uintptr_t) ptr + sz + startingMetadataAlottment - 1 > heap_max) {
        heap_max = (uintptr_t) ptr + sz + startingMetadataAlottment - 1;
    }
    
    ntotal++;
    nactive++;
    active_size += sz;
    total_size += sz;

    // Return pointer to payload, not to start of metadata
    ptr = (void*) ((uintptr_t) ptr + 32);
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

    // Acquire internal metadata
    const char* errmsg = "";

    // Check if ptr is in heap
    if ((uintptr_t) &default_buffer.buffer[32] <= (uintptr_t) ptr && (uintptr_t) ptr <= (uintptr_t) &default_buffer.buffer[8<<20]) {
        startingMetadata* metadataPtr = (startingMetadata*) ((uintptr_t) ptr - 32);
        endingMetadata* endingPtr = (endingMetadata*) ((uintptr_t) ptr + metadataPtr->size);

        if (metadataPtr->allocationKey == '|') {
            // Ensure size stored in bookending metadata matches that which is stored in
            // Starting metdata
            if (endingPtr->size != metadataPtr->size) {
                fprintf(stderr, "MEMORY BUG: %s:%u: detected wild write during free of pointer %p\n", file, line, ptr);
                return;
            } 

            // Detect double free
            if (metadataPtr->freed == true) {
                errmsg = "double free";
            // Detect diabolic wild write
            } else if (activePointers.count((uintptr_t) ptr) == 0) {
                errmsg = "not allocated";
            } else {
                --nactive;
                active_size -= metadataPtr->size;
                metadataPtr->freed = true;
            
                // Update info on contiguous regions w space for allocation
                bool contiguousFound = false;
                size_t sz = metadataPtr->size;
                int alignmentRemainder = (sz + endingMetadataAlottment) % MaxAlignment;
                int alignmentAdjustment = (alignmentRemainder == 0) ? 0 : MaxAlignment - alignmentRemainder;
                
                // Get rid of previous memory
                memset(ptr, '0', sz);
                
                // Combine adjacent free blocks
                startingMetadata* nextBlockMetadata = (startingMetadata*) ((uintptr_t) ptr + sz + endingMetadataAlottment + alignmentAdjustment);
                
                bool nextRegionNeverAllocated = false;
                if ((nextBlockMetadata->allocationKey != '|')) {
                    nextRegionNeverAllocated = true;
                    int i = 0;
                    while (i < 32) {
                        if (((deadMetadata*) nextBlockMetadata)->metadata[i] != 'X') {
                            nextRegionNeverAllocated = false;
                            break;
                        }
                        i++;
                    }
                }

                if ((nextBlockMetadata->allocationKey == '|' && nextBlockMetadata->freed == true) || nextRegionNeverAllocated) {
                    freeRegions[(uintptr_t) ptr - startingMetadataAlottment] = freeRegions[(uintptr_t) nextBlockMetadata] + totalMetadataAlottment + sz + alignmentAdjustment;
                    freeRegions.erase((uintptr_t) nextBlockMetadata);
                    contiguousFound = true;
                }
                
                for (auto& [regionPtr, size] : freeRegions) {
                    if (((uintptr_t) regionPtr + size) == (uintptr_t) ptr - startingMetadataAlottment) {
                        if (contiguousFound) {
                            freeRegions[regionPtr] += freeRegions[(uintptr_t) ptr - startingMetadataAlottment];
                            freeRegions.erase((uintptr_t) ptr - startingMetadataAlottment);
                        } else {
                            freeRegions[regionPtr] += (totalMetadataAlottment + sz + alignmentAdjustment);  
                        }
                        contiguousFound = true;
                        break;
                    }
                }

                if (!contiguousFound) {
                    freeRegions[(uintptr_t) ptr - startingMetadataAlottment] = totalMetadataAlottment + sz + alignmentAdjustment;
                }

                default_buffer.pos = (uintptr_t) ptr - startingMetadataAlottment - (uintptr_t) &default_buffer.buffer[0];
                activePointers.erase((uintptr_t) ptr);
            
            }  
        } else {
            errmsg = "not allocated";
        }
    } else {
        errmsg = "not in heap";
    }
    
    if (strcmp(errmsg, "") != 0) {
        fprintf(stderr, "MEMORY BUG: %s:%u: invalid free of pointer %p, %s\n", file, line, ptr, errmsg);
    }

    if (strcmp(errmsg, "not allocated") == 0) {
            for (auto activePtr : activePointers) {
                startingMetadata* activePtrMetadata = (startingMetadata*) ((uintptr_t) activePtr - 32);
                if ((uintptr_t) activePtr < (uintptr_t) ptr && (uintptr_t) ptr < (uintptr_t) activePtr + activePtrMetadata->size - 1) {
                    fprintf(stderr, "%s:%u: %p is %li bytes inside a %zu byte region allocated here\n", activePtrMetadata->file, activePtrMetadata->line, ptr, (char*) ptr - (char*) activePtr, activePtrMetadata->size);
                }
            }
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
    for (auto activePtr : activePointers) {
        startingMetadata* activePtrMetadata = (startingMetadata*) ((uintptr_t) activePtr - 32);
        fprintf(stdout, "LEAK CHECK: %s:%u: allocated object %p with size %zu\n", activePtrMetadata->file, activePtrMetadata->line, (void*) activePtr, activePtrMetadata->size);
    }
}

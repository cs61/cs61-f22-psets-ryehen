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
// 32 bytes large, so its prepending will always preserve alignment
struct startingMetadata {
    const char* file;                   // file that called for allocation
    size_t size;                        // memory size requested by user
    int alignmentAdjustment;            // adjustment needed to preserve alignment of subsequent block
    int line;                           // line # in file that called for allocation
    bool freed;                         // Bool indicating whether block has been freed
    char allocationKey;                 // Char denoting it's an allocated region
};

// Deliberately of size 16
// May seem redundant, but would make coalescing adjacent blocks of memory easier
// Simply check if prior region has endingMetadata.
// If so, jump back "totalSize - sizeof(endingMetadata)" to check startingMetadata for freed information
// If freed, coalesce
// O(1) time
struct endingMetadata {
    size_t size;
    size_t totalSize;
};

// Constants for greater readability
const int startingMetadataAlottment = 32;
const int endingMetadataAlottment = 16;
const int totalMetadataAlottment = startingMetadataAlottment + endingMetadataAlottment;
const char allocationChar = '|';
const char neverAllocatedChar = 'X';

// Case in which a region was never allocated
// Allows us to check if entire region where metadata should be, lacks it
struct deadMetadata {
    char metadata[startingMetadataAlottment];
};

// Map from ptr to total free region size
std::map<uintptr_t, size_t> freeRegions;

// Set of active pointers
std::unordered_set<uintptr_t> activePointers;

const int MaxAlignment = alignof(std::max_align_t);

// Stores info regarding each block of memory
// 32 bytes large, so its prepending will always preserve alignment
struct startingMetadata {
    const char* file;                   // file that called for allocation
    size_t size;                        // memory size requested by user
    int alignmentAdjustment;            // adjustment needed to preserve alignment of subsequent block
    int line;                           // line # in file that called for allocation
    bool freed;                         // Bool indicating whether block has been freed
    char allocationKey;                 // Char denoting it's an allocated region
};

// Deliberately of size 16
// May seem redundant, but would make coalescing adjacent blocks of memory easier
// Simply check if prior region has endingMetadata.
// If so, jump back "totalSize - sizeof(endingMetadata)" to check startingMetadata for freed information
// If freed, coalesce
// O(1) time
struct endingMetadata {
    size_t size;
    size_t totalSize;
};

// Constants for greater readability
const int startingMetadataAlottment = 32;
const int endingMetadataAlottment = 16;
const int totalMetadataAlottment = startingMetadataAlottment + endingMetadataAlottment;
const char allocationChar = '|';
const char neverAllocatedChar = 'X';

// Case in which a region was never allocated
// Allows us to check if entire region where metadata should be, lacks it
struct deadMetadata {
    char metadata[startingMetadataAlottment];
};

// Map from ptr to total free region size
std::map<uintptr_t, size_t> freeRegions;

// Set of active pointers
std::unordered_set<uintptr_t> activePointers;

// Global stats
unsigned long long nactive = 0;           // number of active allocations [#malloc - #free]
unsigned long long active_size = 0;       // number of bytes in active allocations
unsigned long long ntotal = 0;            // number of allocations, total
unsigned long long total_size = 0;        // number of bytes in allocations, total
unsigned long long nfail = 0;             // number of failed allocation attempts
unsigned long long fail_size = 0;         // number of bytes in failed allocation attempts
uintptr_t heap_min;                       // smallest address in any region ever allocated
uintptr_t heap_max;                       // largest address in any region ever allocated

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

/// m61_malloc(sz, file, line)
///    Returns a pointer to `sz` bytes of freshly-allocated dynamic memory.
///    The memory is not initialized. If `sz == 0`, then m61_malloc may
///    return either `nullptr` or a pointer to a unique allocation.
///    The allocation request was made at source code location `file`:`line`.

// Allows us to only memset 'X' (to indicate never allocated)
// On the very first call to malloc (saves on runtime)
bool firstCallToMalloc = true;
void* m61_malloc(size_t sz, const char* file, int line) {
    (void) file, (void) line;   // avoid uninitialized variable warnings

    void* ptr = nullptr;
    if (nactive == 0) {
        default_buffer.pos = 0;
        
        if (firstCallToMalloc) {
            memset(&default_buffer.buffer[0], 'X', default_buffer.size);
            firstCallToMalloc = false;
        }

        freeRegions.clear();
        activePointers.clear();
        freeRegions[(uintptr_t) &default_buffer.buffer[0]] = 8 << 20;
    }

    // Calculate alignment adjustment required
    int alignmentRemainder = sz % MaxAlignment;
    int alignmentAdjustment = (alignmentRemainder == 0) ? 0 : MaxAlignment - alignmentRemainder;

    // Allocate to first sufficient free region
    bool sufficientBlockFound = false;
    if (freeRegions.size() > 0) {
        for (auto& [regionPtr, regionSize] : freeRegions) {
            if (regionSize >= sz + totalMetadataAlottment + alignmentAdjustment && sz + totalMetadataAlottment + alignmentAdjustment > sz) {
                // Store pointer address
                ptr = (void*) regionPtr;
                sufficientBlockFound = true;
                
                // Update metadata
                activePointers.insert((uintptr_t) regionPtr + startingMetadataAlottment);
                int newRegionSize = regionSize - sz - alignmentAdjustment - totalMetadataAlottment;
                if ( newRegionSize > 0 ) {
                    freeRegions[((uintptr_t) regionPtr + sz + alignmentAdjustment + totalMetadataAlottment)] = newRegionSize;
                }
                freeRegions.erase(regionPtr);
                break;
            }
        }
    }
    
    // If no free region is large enough / available,
    // Update metadata and return nullptr
    if (!sufficientBlockFound) {
        nfail++;
        fail_size += sz;
        return nullptr;
    }
    
    // Store ptr metadata internally
    startingMetadata* startingMetadataPtr = (startingMetadata*) ptr;
    *startingMetadataPtr = (startingMetadata) {file, sz, alignmentAdjustment, line, false, allocationChar};

    endingMetadata* endingMetadataPtr = (endingMetadata*) ((uintptr_t) ptr + startingMetadataAlottment + sz + alignmentAdjustment);
    *endingMetadataPtr = (endingMetadata) {sz, sz + alignmentAdjustment + totalMetadataAlottment};

    // If there was an alignment adjustment after main block, 
    // Fill interceding space that precedes ending metadata with allocationChars
    if (alignmentAdjustment > 0) {
        char* bufferSpace = (char*) ((uintptr_t) ptr + startingMetadataAlottment + sz);

        int i = 0;
        while (i < alignmentAdjustment) {
            *bufferSpace = allocationChar;
            bufferSpace += 1;
            i++;
        }
    }

    // Update max / min heap
    if (!heap_min || (uintptr_t) ptr < heap_min) {
        heap_min = (uintptr_t) ptr;
    }

    if (!heap_max || (uintptr_t) ptr + startingMetadataAlottment + sz - 1 > heap_max) {
        heap_max = (uintptr_t) ptr + startingMetadataAlottment + sz - 1;
    }
    
    ntotal++;
    nactive++;
    active_size += sz;
    total_size += sz;

    // Return pointer to payload, not to start of metadata
    ptr = (void*) ((uintptr_t) ptr + startingMetadataAlottment);
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

    // Check if ptr is in heap
    if ((uintptr_t) &default_buffer.buffer[startingMetadataAlottment] <= (uintptr_t) ptr && (uintptr_t) ptr <= (uintptr_t) &default_buffer.buffer[8<<20]) {
        if ((uintptr_t) ptr % alignof(std::max_align_t) == 0) {
            // Acquire internal metadata
            startingMetadata* metadataPtr = (startingMetadata*) ((uintptr_t) ptr - startingMetadataAlottment);
            endingMetadata* endingPtr = (endingMetadata*) ((uintptr_t) ptr + metadataPtr->size + metadataPtr->alignmentAdjustment );

            if (metadataPtr->allocationKey == allocationChar) {
                // Determine if alignment adjustment buffer space was overwritten
                bool bufferSpaceOverwritten = false;
                if (metadataPtr->alignmentAdjustment != 0) {
                    int i = 0;
                    while (i < metadataPtr->alignmentAdjustment) {
                        if (*((char*) endingPtr - i - 1) != allocationChar) {
                            bufferSpaceOverwritten = true;
                            break;
                        }
                        i++;
                    }
                }

                // Detect wild write
                if (endingPtr->size != metadataPtr->size || bufferSpaceOverwritten) {
                    fprintf(stderr, "MEMORY BUG: %s:%u: detected wild write during free of pointer %p\n", file, line, ptr);
                    abort();
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
                    int alignmentRemainder = sz % MaxAlignment;
                    int alignmentAdjustment = (alignmentRemainder == 0) ? 0 : MaxAlignment - alignmentRemainder;
                    
                    // Get rid of previous memory
                    memset(ptr, '0', sz);
                    
                    // Combine adjacent free blocks
                    startingMetadata* nextBlockMetadata = (startingMetadata*) ((uintptr_t) ptr + sz + endingMetadataAlottment + alignmentAdjustment);
                    
                    // Determine if subsequent region was ever allocated
                    // Technically redundant, since I use "freeRegions.count()" below, but it's indicative of how I would check
                    // If I had had the time to fully remove the freeRegions external metadata
                    bool nextRegionNeverAllocated = false;
                    if ((nextBlockMetadata->allocationKey != allocationChar)) {
                        nextRegionNeverAllocated = true;
                        
                        int i = 0;
                        while (i < startingMetadataAlottment) {
                            if (((deadMetadata*) nextBlockMetadata)->metadata[i] != neverAllocatedChar) {
                                nextRegionNeverAllocated = false;
                                break;
                            }
                            i++;
                        }
                    }

                    // Coalesce with subsequent region as needed
                    if (nextRegionNeverAllocated || freeRegions.count((uintptr_t) nextBlockMetadata) > 0) {
                        freeRegions[(uintptr_t) ptr - startingMetadataAlottment] = freeRegions[(uintptr_t) nextBlockMetadata] + totalMetadataAlottment + sz + alignmentAdjustment;
                        freeRegions.erase((uintptr_t) nextBlockMetadata);
                        contiguousFound = true;
                    }
                    
                    // Coalesce with prior region as needed
                    // Here is where I would have made use of the O(1) algorithm I described above the
                    // "endingMetadata" global struct
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
                    activePointers.erase((uintptr_t) ptr);
                    
                    return;
                } 
            // Didn't have starting metadata allocation key 
            } else {
                errmsg = "not allocated";
            }
        // Alignment was off
        } else {
            errmsg = "not allocated";
        }
    // Wasn't even in heap
    } else {
        errmsg = "not in heap";
    }
    
    fprintf(stderr, "MEMORY BUG: %s:%u: invalid free of pointer %p, %s\n", file, line, ptr, errmsg);

    if (strcmp(errmsg, "not allocated") == 0) {
            for (auto activePtr : activePointers) {
                startingMetadata* activePtrMetadata = (startingMetadata*) ((uintptr_t) activePtr - startingMetadataAlottment);
                if ((uintptr_t) activePtr < (uintptr_t) ptr && (uintptr_t) ptr < (uintptr_t) activePtr + activePtrMetadata->size - 1) {
                    fprintf(stderr, "%s:%u: %p is %li bytes inside a %zu byte region allocated here\n", activePtrMetadata->file, activePtrMetadata->line, ptr, (char*) ptr - (char*) activePtr, activePtrMetadata->size);
                }
            }
        }

    abort();
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
    return stats;
}

/// m61_realloc(ptr, sz, file, line)
///  Reallocates currently allocated memory

void* m61_realloc(void* ptr, size_t sz, const char* file, int line) {
    // Ensure pointer is to a previously allocated block
    // Ensure requested size is greater than zero
    if (activePointers.count((uintptr_t) ptr) == 0 || sz <= 0) {
        nfail++;
        fail_size += sz;
        return nullptr;
    }

    // Check if region can be simply extended
    startingMetadata* oldPtrMetadata = (startingMetadata*) ((uintptr_t) ptr - startingMetadataAlottment);
    uintptr_t adjacentRegionStart = (intptr_t) ptr + oldPtrMetadata->size + oldPtrMetadata->alignmentAdjustment + sizeof(endingMetadata);

    // Calculate new alignment adjustment requirements
    int alignmentRemainder = sz % MaxAlignment;
    int alignmentAdjustment = (alignmentRemainder == 0) ? 0 : MaxAlignment - alignmentRemainder;
    
    int sizeDifference = sz - oldPtrMetadata->size;
    if (sizeDifference < 0) {
        if (alignmentAdjustment > 0) {
                memset((char*) ptr + sz, '|', alignmentAdjustment);
        }

        // Update freeRegions
        if (freeRegions.count(adjacentRegionStart) == 1) {
            freeRegions[(uintptr_t) ptr + sz + alignmentAdjustment + sizeof(endingMetadata)] = freeRegions[adjacentRegionStart] + abs(sizeDifference);
            freeRegions.erase(adjacentRegionStart);
        } else {
            freeRegions[(uintptr_t) ptr + sz + alignmentAdjustment + sizeof(endingMetadata)] = abs(sizeDifference);
        }

    } else if (((oldPtrMetadata->alignmentAdjustment) - sizeDifference - alignmentAdjustment >= 0) || 
                (freeRegions.count(adjacentRegionStart) == 1 && freeRegions[adjacentRegionStart] > (size_t) abs(sizeDifference) + alignmentAdjustment - (oldPtrMetadata->alignmentAdjustment))) {
        
        // Perform alignment adjustment and region extension
        memset((char*) ptr + oldPtrMetadata->size, '0', sizeDifference);
        memset((char*) ptr + sz, '|', alignmentAdjustment);

        // Update freeRegions
        if (freeRegions.count(adjacentRegionStart) == 1) {
            freeRegions[(uintptr_t) ptr + sz + alignmentAdjustment + sizeof(endingMetadata)] = freeRegions[adjacentRegionStart] - abs(sizeDifference);
            freeRegions.erase(adjacentRegionStart);
        }
    } else {
        // Call malloc to create memory region with newly requested size
        void* newPtr = m61_malloc(sz, file, line);

        if (newPtr == nullptr) {
            nfail++;
            fail_size += sz;
            return nullptr;
        }
    
        // Copy data from old ptr to new ptr
        size_t i = 0;
        while (i < oldPtrMetadata->size && i < sz) {
            char transferByte = *((char*) ((uintptr_t) ptr + i));
            char* byteDestinationAddress = (char*) ((uintptr_t) newPtr + i);
            
            memset(byteDestinationAddress, transferByte, 1);
            i++;
        }

        // Free previously allocated block
        m61_free(ptr);
        return newPtr;
    }
    // This area is only reached if region was extended or truncated
    // Update metadata
    oldPtrMetadata->size = sz;
    oldPtrMetadata->alignmentAdjustment = alignmentAdjustment;
    oldPtrMetadata->file = file;
    oldPtrMetadata->line = line;

    *((endingMetadata*) ((char*) ptr + sz + alignmentAdjustment)) = {sz, sz + alignmentAdjustment + sizeof(endingMetadata)};
    
    active_size += sizeDifference;
    total_size += sizeDifference;
    return ptr;    
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
        startingMetadata* activePtrMetadata = (startingMetadata*) ((uintptr_t) activePtr - startingMetadataAlottment);
        fprintf(stdout, "LEAK CHECK: %s:%u: allocated object %p with size %zu\n", activePtrMetadata->file, activePtrMetadata->line, (void*) activePtr, activePtrMetadata->size);
    }
}

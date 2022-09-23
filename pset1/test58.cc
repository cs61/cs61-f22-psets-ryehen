#include "m61.hh"
#include <cstdio>
#include <cassert>
#include <cstring>
#include <iostream>
#include <time.h>
// Ensure realloc performs data transfer
int main() {
    const char* oldString = "Hey, what's up guys? How are you?";
    int oldSize = strlen(oldString) + 1;
    char* oldPtr = (char*) m61_malloc(oldSize);
    assert(oldPtr != nullptr);
    strcpy(oldPtr, oldString);

    // initialize random seed
    srand (time(NULL));
    // Get new, random size between 1 and 10 greater than the length of the old string
    int newSize = rand() % (strlen(oldString) + 11) + 1;
    
    // Realloc
    char* newPtr = (char*) m61_realloc((void*) oldPtr, newSize);
    assert(newPtr != nullptr);

    // Cant use strcmp because the new, shorter memory region won't have \0 char
    // Compare char by char
    int i = 0;
    while (i < newSize && i < oldSize) {
        assert(newPtr[i] == oldString[i]);
        i++;
    }
    m61_print_statistics();
}


//! alloc count: active          1   total          2   fail          0
//! alloc size:  active        ???   total        ???   fail          0

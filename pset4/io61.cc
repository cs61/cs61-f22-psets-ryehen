#include "io61.hh"
#include <iostream>
#include <unordered_map>
#include <sys/types.h>
#include <sys/stat.h>
#include <climits>
#include <cerrno>

// io61.cc
// cache
struct io61_fcache {
    int fd = -1;                                // File descriptor of file associated w/cache
    static constexpr off_t bufsize = 8192;      // Each block is 8192 bytes
    size_t filesize = -1;                       // Size of file cached
    unsigned char cbuf[bufsize];                // Cached data is stored in `cbuf`
    off_t tag = 0;                              // `tag`: File offset of first byte of cached data (0 when file is opened).
    off_t end_tag = 0;                          // `end_tag`: File offset one past the last byte of cached data (0 when file is opened).
    off_t pos_tag = 0;                          // `pos_tag`: Cache position: file offset of the cache.
    int mode;                                   // File mode (read or write)
    bool reversing = false;                     // Bool detects if previous lseek decreased file pos
    bool just_sought = false;                   // Bool detects if we got to current file pos via a seek
    bool final_flush = false;                   // Bool detects if final flush before closing file
};

// Initialize maps from file descriptor to associated cache
std::unordered_map<int, io61_fcache*> read_caches;
std::unordered_map<int, io61_fcache*> write_caches;

// io61_file
//    Data structure for io61 file wrappers. Add your own stuff.

struct io61_file {
    int fd = -1;     // file descriptor
    int mode;        // File mode (read or write)
};


// io61_fdopen(fd, mode)
//    Returns a new io61_file for file descriptor `fd`. `mode` is either
//    O_RDONLY for a read-only file or O_WRONLY for a write-only file.
//    You need not support read/write files.

io61_file* io61_fdopen(int fd, int mode) {
    assert(fd >= 0);
    io61_file* f = new io61_file;
    f->fd = fd;
    f->mode = mode;
    if (mode == O_RDONLY) {
        read_caches[fd] = new io61_fcache;
        read_caches[fd]->fd = fd;
        read_caches[fd]->mode = O_RDONLY;
        read_caches[fd]->filesize = io61_filesize(f);
    } else {
        write_caches[fd] = new io61_fcache;
        write_caches[fd]->fd = fd;
        write_caches[fd]->mode = O_WRONLY;
        write_caches[fd]->filesize = io61_filesize(f);
    }
    return f;
}


// io61_close(f)
//    Closes the io61_file `f` and releases all its resources.

int io61_close(io61_file* f) {
    int fd = f->fd;
    if (f->mode == O_WRONLY) {
        if (write_caches[fd]->tag != write_caches[fd]->pos_tag) {
            write_caches[fd]->final_flush = true;
            io61_flush(f);
        }
        delete write_caches[fd];
        write_caches.erase(fd);
    } else {
        delete read_caches[fd];
        read_caches.erase(fd);
    }
    int r = close(f->fd);
    delete f;
    return r;
}

// io61_fill(f)
//   Empties cache entry & then fills it with new data
//   Returns number of bytes read

int io61_fill(io61_fcache* f) {
    // Check invariant
    assert(abs(f->end_tag - f->pos_tag) <= f->bufsize);

    // Reset cache to empty
    f->tag = f->pos_tag = f->end_tag;

    // If file position is within bufsize bytes of end of file,
    // And a seek was performed to get there—get the last bufsize bytes (may be reversing!)
    off_t adjusted_pos = f->end_tag;
    if (f->filesize - f->bufsize < f->end_tag && f->just_sought) {
        adjusted_pos = lseek(f->fd, f->filesize - f->bufsize, SEEK_SET);
    // If our last io61_seek was a reverse, we get bufsize/2 bytes before current pos
    // and bufsize/2 bytes after current pos 
    // (good chance next read will be before current pos, but may not be!)
    } else if (f->reversing) {
        adjusted_pos = lseek(f->fd, f->end_tag - (f->bufsize/2 - 1), SEEK_SET);
    } 

    ssize_t n = read(f->fd, f->cbuf, f->bufsize);
    //std::cout << n << std::endl;
    if (n >= 0) {
        f->end_tag = adjusted_pos + n;
        f->tag = adjusted_pos;
    }
    // Recheck invariants
    assert(abs(f->end_tag - f->pos_tag) <= f->bufsize);
    return n;
}


// io61_readc(f)
//    Reads a single (unsigned) byte from `f` and returns it. Returns EOF,
//    which equals -1, on end of file or error.

int io61_readc(io61_file* f) {
    // Initialize buffer for char
    unsigned char buf[1];

    // Acquire associated read cache and ensure it's filled
    io61_fcache* read_cache = read_caches[f->fd];
    if (read_cache->pos_tag == read_cache->end_tag) {
        int nfilled =  io61_fill(read_cache);
        if (nfilled == 0) {
            return -1;
        }
    }

    // Acquire next char from read cache
    buf[0] = read_cache->cbuf[read_cache->pos_tag - read_cache->tag];


    // Either increment or decerement pos tag depending on likelihood of reversal
    // (we are more likely to reverse if we're at the end of the file or our last lseek decremented pos)
    //if (read_cache->pos_tag + 1 != read_cache->filesize) {
        ++read_cache->pos_tag;
    //}
    read_cache->just_sought = false;
    return buf[0];
}


// io61_read(f, buf, sz)
//    Reads up to `sz` bytes from `f` into `buf`. Returns the number of
//    bytes read on success. Returns 0 if end-of-file is encountered before
//    any bytes are read, and -1 if an error is encountered before any
//    bytes are read.
//
//    Note that the return value might be positive, but less than `sz`,
//    if end-of-file or error is encountered before all `sz` bytes are read.
//    This is called a “short read.”

ssize_t io61_read(io61_file* f, unsigned char* buf, size_t sz) {
    io61_fcache* read_cache = read_caches[f->fd];

    size_t pos = 0;
    off_t buffer_offset = 0;
    while (pos < sz) {
        // Fill cache
        if (read_cache->pos_tag == read_cache->end_tag) {
            int nfilled =  io61_fill(read_cache);
            if (nfilled == 0) {
                break;
            }
        }
        
        // Caclulate how many bytes are in cache
        size_t bytes_in_cache = read_cache->end_tag - read_cache->tag;

        // Caclulate how many bytes we can read
        size_t bytes_readable = std::min(bytes_in_cache, sz - pos);

        // Calculate bytes currently unread in our cache
        size_t bytes_unread = read_cache->end_tag - read_cache->pos_tag;

        // Use aforementioned values to determine the quantity of bytes we read in this pass
        size_t bytes_to_read = std::min(bytes_readable, bytes_unread);
        
        // Read from cache
        memcpy(&buf[buffer_offset], &read_cache->cbuf[read_cache->pos_tag - read_cache->tag], bytes_to_read);
        if (bytes_to_read < sz) {
            buffer_offset = bytes_to_read;
        }
        read_cache->pos_tag += bytes_to_read;
        read_cache->just_sought = false;                  // Our current cache position is not the result of an io61_seek call
        pos += bytes_to_read;
    }

    if (pos != 0 || sz == 0 || errno == 0) {
        return pos;
    } else {
        return -1;
    }
}


// io61_writec(f)
//    Write a single character `ch` to `write_cache`. Returns 0 on success and
//    -1 on error.

int io61_writec(io61_file* f, int ch) {
    // Acquire associated write cache
    io61_fcache* write_cache = write_caches[f->fd];

    // Determine if we're reversing or not
    off_t buffer_pos = write_cache->end_tag - write_cache->tag;
    bool reversing = false;
    if (buffer_pos < 0) {
        buffer_pos = write_cache->bufsize - 1 + buffer_pos;
        reversing = true;
    }

    // If cache is full, flush before copying new char to it
    if ((buffer_pos == write_cache->bufsize && !reversing) || (buffer_pos == -1 && reversing)){
        if (io61_flush(f) == -1) {
            return -1;
        }

        if (!reversing) {
            buffer_pos = write_cache->end_tag - write_cache->tag;
        }
        // if (write_cache->pos_tag == 0) {
        //     return 0;
        // }
    }

    if (buffer_pos == -1 && write_cache->just_sought)  {
        buffer_pos = write_cache->bufsize - 1;
    }

    
    ////std::cerr << "writing start tag: " << write_cache->tag << std::endl;
    ////std::cerr << "writing end tag: " << write_cache->end_tag << std::endl;
    ////std::cerr << "writing pos: " << write_cache->pos_tag << std::endl;

    ////std::cerr << "after pos: " << write_cache->end_tag - write_cache->tag << std::endl;
    ////std::cerr << "file pos: " << lseek(write_cache->fd, 0, SEEK_CUR) << std::endl;
    // ////std::cerr << "char written: " << (char) ch << std::endl;

    write_cache->cbuf[buffer_pos] = ch;
    //////std::cerr << write_cache->cbuf[0] << std::endl;

    // In this case, we don't know if what we're currently writing should be seen as the beginning
    // Or the end of the buffer, so we put it in both places
    if (write_cache->just_sought && write_cache->pos_tag == write_cache->tag) {
        write_cache->cbuf[write_cache->bufsize - 1] = ch;
    }

    write_cache->just_sought = false; 
    // If cache was filled, flush
    if (buffer_pos == 0 && reversing) {
        if (io61_flush(f) == -1) {
            return -1;
        } else {
            return 0;
        }
    }

    if (reversing) {
        --write_cache->pos_tag;
        --write_cache->end_tag;
    } else {
        ++write_cache->pos_tag;
        ++write_cache->end_tag;
    }
     
    return 0;
}


// io61_write(f, buf, sz)
//    Writes `sz` characters from `buf` to `f`. Returns `sz` on success.
//    Can write fewer than `sz` characters when there is an error, such as
//    a drive running out of space. In this case io61_write returns the
//    number of characters written, or -1 if no characters were written
//    before the error occurred.

ssize_t io61_write(io61_file* f, const unsigned char* buf, size_t sz) {
    io61_fcache* write_cache = write_caches[f->fd];

    size_t nwritten = 0;
    off_t buffer_offset = 0;
    // std::cerr << "write buf: " << std::endl << write_cache->cbuf << std::endl << std::endl;
    while (nwritten != sz) {
        // Determine how much space is left in write cache
        size_t bytes_writable = write_cache->tag + write_cache->bufsize - write_cache->pos_tag;
        
        // Determine how many bytes we'll write
        size_t bytes_to_write = std::min(bytes_writable, sz - nwritten);

        // Copy from buffer to write cache
        memcpy(&write_cache->cbuf[write_cache->pos_tag - write_cache->tag], &buf[buffer_offset], bytes_to_write);
        if (bytes_to_write < sz) {
            buffer_offset = bytes_to_write;
        }

        // Increment cache file positions accordingly
        write_cache->pos_tag += bytes_to_write;
        write_cache->end_tag += bytes_to_write;
        write_cache->just_sought = false;                  // Our current cache position is not the result of an io61_seek call
        nwritten += bytes_to_write;

        if (write_cache->pos_tag - write_cache->tag == write_cache->bufsize) {
            if (io61_flush(f) == -1) {
                return -1;
            }
        }
    }

    if (nwritten != 0 || sz == 0) {
        return nwritten;
    } else {
        return -1;
    }
}


// io61_flush(f)
//    Forces a write of any cached data written to `f`. Returns 0 on
//    success. Returns -1 if an error is encountered before all cached
//    data was written.
//
//    If `f` was opened read-only, `io61_flush(f)` returns 0. If may also
//    drop any data cached for reading.

int io61_flush(io61_file* f) {
    //std::cerr<<"flushing"<<std::endl;
    io61_fcache* cache = nullptr;
    bool reversing = false;
    if (f->mode == O_WRONLY) {
        // Get cache
        cache = write_caches[f->fd];
        
        // Check invariants.
        assert(abs(cache->pos_tag - cache->tag) <= cache->bufsize);
        assert(cache->pos_tag == cache->end_tag);

        // Determine how many bytes to write
        int bytes_to_write = cache->pos_tag - cache->tag;
        if (bytes_to_write < 0) {
            reversing = true;
            bytes_to_write = abs(bytes_to_write) + 1;
        }

        // Write from cache to file
        ssize_t nwritten = 0;
        nwritten = write(f->fd, &cache->cbuf[nwritten], bytes_to_write);

        // Check all bytes were written from cache
        if (nwritten != bytes_to_write) {
            if (errno == 0 || cache->final_flush) {
                nwritten = std::max((int) nwritten, 0);
                off_t total_written = nwritten;
                int sign_corrector = 1;
                if (cache->tag > cache->pos_tag) {
                    sign_corrector = -1;
                }
                cache->tag += nwritten * sign_corrector;
                while (cache->tag != cache->end_tag) {
                    nwritten = write(f->fd, &cache->cbuf[total_written], abs(cache->pos_tag - cache->tag));
                    cache->tag += std::max((int) nwritten, 0) * sign_corrector;
                    total_written += std::max((int) nwritten, 0) * sign_corrector;
                }
            } else {
                return -1;
            }
        }
    } else {
        cache = read_caches[f->fd];
    }
    
    if (reversing) {
        --cache->pos_tag;
    }
    cache->tag = cache->pos_tag;
    cache->end_tag = cache->pos_tag;
    return 0;
}

int io61_seek_forward(io61_fcache* cache, off_t pos);
int io61_seek_backward(io61_fcache* cache, off_t pos);

// io61_seek(f, pos)
//    Changes the file pointer for file `f` to `pos` bytes into the file.
//    Returns 0 on success and -1 on failure.

int io61_seek(io61_file* f, off_t pos) {
    // Determine which cache needs to be updated
    io61_fcache* cache = nullptr;
    if (f->mode == O_RDONLY) {
        cache = read_caches[f->fd];
    } else {
        cache = write_caches[f->fd];
    }

    assert(f->fd == cache->fd);
    
    // Detect if reversing or not and seek accordingly
    if (pos == cache->pos_tag) {
        if (cache->reversing) {
            int r = lseek(cache->fd, pos, SEEK_SET);
            if ( r != pos) {
                return -1;
            }
        }
        cache->just_sought = true;
        return 0;
    } else if (pos < cache->pos_tag) {
        return io61_seek_backward(cache, pos);
    } else {
        return io61_seek_forward(cache, pos);
    }
}

int io61_seek_forward(io61_fcache* cache, off_t pos) {
    // Get current file position
    off_t current_fpos = lseek(cache->fd, 0, SEEK_CUR);

    // Update cache pos
    cache->pos_tag = pos;

    // Mark cache empty if the seek set our position
    // after the end tag
    if (cache->pos_tag > cache->end_tag) {
        cache->end_tag = pos;
        cache->tag = pos;
    }

    // If cache is empty, we'll need to lseek actual file too
    if (cache->tag == cache->pos_tag && cache->pos_tag == cache->end_tag) {
        current_fpos = lseek(cache->fd, pos, SEEK_SET);

        if (current_fpos != pos) {
            return -1;
        }
    }

    // Check invariants
    assert((cache->pos_tag <= current_fpos && cache->mode == O_RDONLY) || 
            (cache->pos_tag >= current_fpos && cache->mode == O_WRONLY));

    // Update cache metadata
    cache->reversing = false;
    cache->just_sought = true;

    return 0;
}

int io61_seek_backward(io61_fcache* cache, off_t pos) {
    // Get current file position
    off_t current_fpos = lseek(cache->fd, 0, SEEK_CUR);

    // Update cache pos
    cache->pos_tag = pos;

    // Shift cache if seek set our position before
    // before the start tag
    if (pos < cache->tag) {
        if (pos < cache->tag - cache->bufsize) {
            cache->tag = pos;
        }
        cache->end_tag = pos;
        current_fpos = lseek(cache->fd, pos, SEEK_SET);
        
        if (current_fpos != pos) {
            return -1;
        }
    }

    // Update cache metadata
    cache->reversing = true;
    cache->just_sought = true;
    return 0;
}


// You shouldn't need to change these functions.

// io61_open_check(filename, mode)
//    Opens the file corresponding to `filename` and returns its io61_file.
//    If `!filename`, returns either the standard input or the
//    standard output, depending on `mode`. Exits with an error message if
//    `filename != nullptr` and the named file cannot be opened.

io61_file* io61_open_check(const char* filename, int mode) {
    int fd;
    if (filename) {
        fd = open(filename, mode, 0666);
    } else if ((mode & O_ACCMODE) == O_RDONLY) {
        fd = STDIN_FILENO;
    } else {
        fd = STDOUT_FILENO;
    }
    if (fd < 0) {
        fprintf(stderr, "%s: %s\n", filename, strerror(errno));
        exit(1);
    }
    return io61_fdopen(fd, mode & O_ACCMODE);
}


// io61_fileno(f)
//    Returns the file descriptor associated with `f`.

int io61_fileno(io61_file* f) {
    return f->fd;
}


// io61_filesize(f)
//    Returns the size of `f` in bytes. Returns -1 if `f` does not have a
//    well-defined size (for instance, if it is a pipe).

off_t io61_filesize(io61_file* f) {
    struct stat s;
    int r = fstat(f->fd, &s);
    if (r >= 0 && S_ISREG(s.st_mode)) {
        return s.st_size;
    } else {
        return -1;
    }
}

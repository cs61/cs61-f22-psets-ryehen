#include "io61.hh"
#include <sys/types.h>
#include <sys/stat.h>
#include <climits>
#include <cerrno>
#include <iostream>
#include <unordered_map>

// io61.cc
//    YOUR CODE HERE!

// Define NUL character for /dev/zero
#define NUL '\0'

// io61_cache
//    Data structure for io61 caches
struct io61_fcache {
    int fd = -1;                                // File descriptor of file associated w/cache
    static constexpr off_t bufsize = 8192;      // Each block is 8192 bytes
    size_t filesize = -1;                       // Size of file cached
    unsigned char cbuf[bufsize];                // Cached data is stored in `cbuf`
    off_t tag = 0;                              // `tag`: File offset of first byte of cached data (0 when file is opened).
    off_t end_tag = 0;                          // `end_tag`: File offset one past the last byte of cached data (0 when file is opened).
    off_t pos_tag = 0;                          // `pos_tag`: Cache position: file offset of the cache.
    int mode;                                   // File mode (read or write)
    bool is_dev_zero = false;
};

// Initialize maps from file descriptor to associated cache
std::unordered_map<int, io61_fcache*> read_caches;
std::unordered_map<int, io61_fcache*> write_caches;

// io61_file
//    Data structure for io61 file wrappers. Add your own stuff.

struct io61_file {
    int fd = -1;     // file descriptor
    int mode;        // file mode (read or write)
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

// io61_fill(cache)
//   Fills a read cache with chars
//   Returns number of chars filled
int io61_fill(io61_fcache* cache) {
    // Check invariant
    assert(cache->end_tag - cache->pos_tag <= cache->bufsize);

    // Reset cache to empty
    cache->tag = cache->pos_tag = cache->end_tag;

    // Fill cache
    ssize_t nfilled = read(cache->fd, cache->cbuf, cache->bufsize);

    // Update end tag accordingly
    cache->end_tag = cache->tag + nfilled;

    // Check invariant
    assert(cache->end_tag - cache->pos_tag <= cache->bufsize);
    return nfilled;
}


// io61_readc(f)
//    Reads a single (unsigned) byte from `f` and returns it. Returns EOF,
//    which equals -1, on end of file or error.

int io61_readc(io61_file* f) {
    // Initialize buffer for char
    unsigned char buf[1];

    // Acquire file's cache and ensure it's filled
    io61_fcache* read_cache = read_caches[f->fd];

    // If empty, refill read cache
    if (read_cache->pos_tag == read_cache->end_tag) {
        int nfilled =  io61_fill(read_cache);
        if (nfilled == 0) {
            errno = 0;  // clear `errno` to indicate EOF
            return -1;
        }
    }

    // If cache is associated with dev zero
    if (read_cache->is_dev_zero) {
        return NUL;
    }

    // Acquire next char from read cache
    buf[0] = read_cache->cbuf[read_cache->pos_tag - read_cache->tag];

    // Increment cache pos
    ++read_cache->pos_tag;

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
    // Acquire associated cache
    io61_fcache* read_cache = read_caches[f->fd];

    size_t nread = 0;
    off_t buffer_offset = 0;
    while (nread != sz) {
        // Fill cache as needed
        if (read_cache->pos_tag == read_cache->end_tag) {
            int nfilled = io61_fill(read_cache);
            if (nfilled == 0) {
                break;
            }
        }

        // Caclulate how many bytes are in cache
        size_t bytes_in_cache = read_cache->end_tag - read_cache->tag;

        // Caclulate how many bytes are readable in total
        size_t total_bytes_readable = std::min(bytes_in_cache, sz - nread);

        // Calculate bytes currently unread in our cache
        size_t bytes_unread = read_cache->end_tag - read_cache->pos_tag;

        // Use above values to determine the quantity of bytes we read in this pass
        size_t bytes_to_read = std::min(total_bytes_readable, bytes_unread);

        // Read from cache
        memcpy(&buf[buffer_offset], &read_cache->cbuf[read_cache->pos_tag - read_cache->tag], bytes_to_read);

        // Update offset as needed
        if (bytes_to_read < sz) {
            buffer_offset = bytes_to_read;
        }
        read_cache->pos_tag += bytes_to_read;
        nread += bytes_to_read;
    }

    if (nread != 0 || sz == 0 || errno == 0) {
        return nread;
    } else {
        return -1;
    }
}


// io61_writec(f)
//    Write a single character `ch` to `f`. Returns 0 on success and
//    -1 on error.

int io61_writec(io61_file* f, int ch) {
    // Acquire associated write cache
    io61_fcache* write_cache = write_caches[f->fd];

    // Check invariant
    assert(write_cache->pos_tag == write_cache->end_tag);

    // If cache is full, flush before copying new char to it
    if (write_cache->pos_tag - write_cache->tag == write_cache->bufsize) {
         if (io61_flush(f) == -1) {
            return -1;
        }
    }

    // Copy char to write cache
    write_cache->cbuf[write_cache->pos_tag - write_cache->tag] = ch;
    
    // Increment cache pos
    ++write_cache->pos_tag;
    ++write_cache->end_tag;

    // Recheck invariant
    assert(write_cache->pos_tag == write_cache->end_tag);

    return 0;
}


// io61_write(f, buf, sz)
//    Writes `sz` characters from `buf` to `f`. Returns `sz` on success.
//    Can write fewer than `sz` characters when there is an error, such as
//    a drive running out of space. In this case io61_write returns the
//    number of characters written, or -1 if no characters were written
//    before the error occurred.

ssize_t io61_write(io61_file* f, const unsigned char* buf, size_t sz) {
    // Acquire associated write cache
    io61_fcache* write_cache = write_caches[f->fd];

    size_t nwritten = 0;
    off_t buffer_offset = 0;
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
        nwritten += bytes_to_write;

        // Flush cache if full
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
    io61_fcache* cache = nullptr;
    if (f->mode == O_WRONLY) {
        // Acquire associated cache
        cache = write_caches[f->fd];

        // Check invariants.
        assert(cache->pos_tag - cache->tag <= cache->bufsize);
        assert(cache->pos_tag == cache->end_tag);

        // Determine how many bytes to write
        int bytes_to_write = cache->pos_tag - cache->tag;

        // Write from cache to file
        ssize_t nwritten = write(f->fd, &cache->cbuf, bytes_to_write);

        // Check all bytes were written from cache
        if (nwritten != bytes_to_write) {
            if (errno == EINTR || errno == EAGAIN) {
                nwritten = std::max((int) nwritten, 0);
                int total_written = nwritten;
                cache->tag += nwritten;
                while (cache->tag != cache->end_tag) {
                    nwritten = write(f->fd, &cache->cbuf[total_written], cache->pos_tag - cache->tag);
                    nwritten = std::max((int) nwritten, 0);
                    cache->tag += nwritten;
                    total_written += nwritten;
                }
            } else {
                return -1;
            }
        }
    } else {
        // Acquire associated cache
        cache = read_caches[f->fd];
    }
    // Mark cache empty
    cache->tag = cache->pos_tag = cache->end_tag;
    return 0;
}

int io61_seek_read(io61_fcache* cache, off_t pos);
int io61_seek_write(io61_file* f, io61_fcache* cache, off_t pos);

// io61_seek(f, pos)
//    Changes the file pointer for file `f` to `pos` bytes into the file.
//    Returns 0 on success and -1 on failure.

int io61_seek(io61_file* f, off_t pos) {
    // Determine which cache needs to be updated
    if (f->mode == O_RDONLY) {
        return io61_seek_read(read_caches[f->fd], pos);
    } else {
        return io61_seek_write(f, write_caches[f->fd], pos);
    }
}

int io61_seek_read(io61_fcache* cache, off_t pos) {
    // Determine if entire cache needs to be shifted
    if (pos < cache->tag || cache->end_tag < pos) {
        off_t seek_pos = std::max(pos - (off_t) cache->bufsize/2, (off_t) 0);
        off_t sought_pos = lseek(cache->fd, seek_pos, SEEK_SET);
        if (sought_pos == seek_pos) {
            // Make sure we refill cache from correct position
            cache->tag = cache->pos_tag = cache->end_tag = sought_pos;
            int nfilled = io61_fill(cache);
            cache->pos_tag = pos;
            
            // Check invariants
            assert(cache->end_tag == sought_pos + nfilled);
            assert(cache->tag <= cache->pos_tag);
        } else if (sought_pos == 0) {
            cache->is_dev_zero = true;
        } else {
            return -1;
        }
    }

    // Otherwise, just update cache pos
    cache->pos_tag = pos;
    return 0;
}
int io61_seek_write(io61_file* f, io61_fcache* cache, off_t pos) {
    // Determine if entire cache needs to be shifted
    if (pos < cache->tag || cache->end_tag < pos) {
        if (cache->pos_tag - cache->tag > 0) {
            io61_flush(f);
        }
        off_t sought_pos = lseek(cache->fd, pos, SEEK_SET);
        if (sought_pos == pos) {
            cache->tag = cache->pos_tag = cache->end_tag = pos;

            assert(cache->end_tag == cache->pos_tag);
        } else if (sought_pos == 0) { 
            cache->is_dev_zero = true;
        } else {
            return -1;
        }
    }

    // Otherwise, update cache pos and end tag
    cache->pos_tag = cache->end_tag = pos;
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

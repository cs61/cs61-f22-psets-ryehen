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
    // Check invariants
    assert(f->tag <= f->pos_tag && f->pos_tag <= f->end_tag);
    assert(f->end_tag - f->pos_tag <= f->bufsize);
    //assert(f->end_tag == lseek(f->fd, 0, SEEK_CUR));

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
    // std::cerr<< "n read: " << n << std::endl;
    // std::cerr << "file size: " << f->filesize << std::endl;
    // std::cerr << "pos: " << f->pos_tag << std::endl;
    // std::cerr << "end tag: " << f->end_tag <<std::endl << std::endl; 
    // std::cerr << "adjusted pos: " << adjusted_pos <<std::endl << std::endl; 
    // Recheck invariants (good practice!).
    assert(f->tag <= f->pos_tag && f->pos_tag <= f->end_tag);
    assert(f->end_tag - f->pos_tag <= f->bufsize);
    //assert(f->end_tag == lseek(f->fd, 0, SEEK_CUR));

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
    // std::cerr << read_cache->pos_tag << std::endl; 
    // std::cerr << read_cache->tag  << std::endl;
    // std::cerr << buf[0] << std::endl;
    // std::cerr << read_cache->cbuf[8191] << std::endl;
    //assert(false);

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
            //std::cerr << nfilled << std::endl;
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
    // Initialize buffer for char
    unsigned char buf[1];

    buf[0] = ch;
    ssize_t nw = write(f->fd, buf, 1);
    if (nw == 1) {
        return 0;
    } else {
        return -1;
    }
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
    //std::cout << "write buf: " << std::endl << write_cache->cbuf << std::endl << std::endl;
    while (nwritten != sz) {
        // Determine how much space is left in write cache
        size_t bytes_writable = write_cache->tag + write_cache->bufsize - write_cache->pos_tag;
        
        // Determine how many bytes we'll write
        size_t bytes_to_write = std::min(bytes_writable, sz - nwritten);

        // if (bytes_to_write == 0) {
        //     return 0;
        // }

        // Copy from buffer to write cache
        //std::cout << bytes_to_write << std::endl;
        memcpy(&write_cache->cbuf[write_cache->pos_tag - write_cache->tag], &buf[buffer_offset], bytes_to_write);
        if (bytes_to_write < sz) {
            buffer_offset = bytes_to_write;
        }
        //std::cout << write_cache->cbuf << std::endl << std::endl;
        //std::cout << buf << std::endl;

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
    io61_fcache* write_cache = write_caches[f->fd];
    if (f->mode == O_WRONLY) {
        // Check invariants.
        assert(write_cache->tag <= write_cache->pos_tag && write_cache->pos_tag <= write_cache->end_tag);
        assert(write_cache->end_tag - write_cache->pos_tag <= write_cache->bufsize);

        // Write cache invariant.
        assert(write_cache->pos_tag == write_cache->end_tag);

        // Write from cache to file
        ssize_t n = write(f->fd, write_cache->cbuf, write_cache->pos_tag - write_cache->tag);

        // Check all bytes were written from cache
        if (n != write_cache->pos_tag - write_cache->tag) {
            return -1;
        }
        write_cache->tag = write_cache->pos_tag;
    }
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
    if (pos < cache->pos_tag) {
        return io61_seek_backward(cache, pos);
    } else {
        return io61_seek_forward(cache, pos);
    }
}

int io61_seek_forward(io61_fcache* cache, off_t pos) {
    // Get current file position
    off_t current_fpos = lseek(cache->fd, 0, SEEK_CUR);

    // Check invariants
    assert((cache->pos_tag <= current_fpos && cache->mode == O_RDONLY) || 
            (cache->pos_tag >= current_fpos && cache->mode == O_WRONLY));

    // Update cache pos
    cache->pos_tag = pos;

    // Mark cache empty if the seek set our position
    // after the end tag
    if (cache->pos_tag > cache->end_tag) {
        cache->end_tag = pos;
        cache->tag = pos;
    }

    // If cache is empty, we'll need to lseek actual file too
    if (cache->pos_tag > current_fpos && cache->mode == O_RDONLY) {
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

    // Mark cache empty if the seek set our position
    // before the start tag
    if (cache->pos_tag < cache->tag) {
        cache->end_tag = pos;
        cache->tag = pos;
    }

    // If cache is empty, we'll need to lseek actual file too
    if (cache->pos_tag < current_fpos - cache->bufsize && cache->mode == O_RDONLY) {
        current_fpos = lseek(cache->fd, pos, SEEK_SET);
        //io61_fill(cache);
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

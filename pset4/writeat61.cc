#include "io61.hh"

// Usage: ./writeat61 [-s SIZE] [-p POS] [-o OUTFILE] [FILE]
//    Copies the input FILE to OUTFILE one character at a time,
//    starting at position POS.
//    Reads using stdio and writes using io61.

int main(int argc, char* argv[]) {
    // Parse arguments
    io61_args args = io61_args("s:o:i:p:Fy").parse(argc, argv);

    FILE* inf = stdio_open_check(args.input_file, O_RDONLY);
    io61_file* outf = io61_open_check(args.output_file, O_WRONLY | O_CREAT);
    args.after_open(inf, O_RDONLY);

    int r = io61_seek(outf, args.initial_offset);
    assert(r == 0);

    while (args.file_size > 0) {
        int ch = fgetc(inf);
        if (ch == EOF) {
            break;
        }

        r = io61_writec(outf, ch);
        assert(r == 0);
        --args.file_size;

        args.after_write(outf);
    }

    fclose(inf);
    io61_close(outf);
}

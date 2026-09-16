// The program. Build it with every piece named at once:
//
//     ceresc examples/interop/io.c examples/interop/hello.c \
//            examples/interop/triple.casm examples/interop/banner.casm \
//            -o examples/interop/hello.cres --run
//
// ceresc compiles each .c, writes a declarations file the units import so they can name each
// other's symbols, assembles every .casm (generated and hand-written alike) into an object, links
// them, and runs the result. See docs/07-CASM-Interop.md.

#include "io.h"

// Written in assembly, in triple.casm. `extern` says so: defined elsewhere, no storage here.
extern int triple(int n);
extern void showBanner(void);

// `static` keeps this out of the linker's table entirely - nothing outside this file can call it.
static void line(const char* label, int value)
{
    putstr(label);
    putint(value);
    put('\n');
}

// `inline` is a request the optimizer honours: with inlining on (-O2), a body this small is spliced
// into the call instead of being called. The function is still emitted and still callable.
inline int square(int n)
{
    return n * n;
}

int main(void)
{
    showBanner();
    putstr("hello from C and CASM\n");

    line("triple(7)     = ", triple(7));
    line("square(9)     = ", square(9));

    // `writeCount` lives in io.c and is declared `extern` in io.h - one object, two files.
    line("bytes written = ", writeCount);
    return 0;
}

// 33 - a byte-copy loop lowered to the compiler's own word-at-a-time routine.
//
// `copy_bytes` is the canonical `for (i = 0; i < n; i++) d[i] = s[i];`. Loop-idiom recognition
// (docs/14 O15) replaces it with a call to `__cc_memcpy`, a routine the compiler emits into the
// same unit (never linked from the library).
//
// A copy is subtler than a fill: the C loop is a FORWARD byte copy, and it is well defined even
// when the two ranges overlap - with `d > s` it repeats bytes rather than reproducing the source,
// which `memcpy`/`memmove` would not do. So `__cc_memcpy` copies words only where that is provably
// the same (`d <= s`, or the ranges disjoint) and falls back to a faithful forward byte copy for the
// `d > s` overlap. The output below is the same at -O0, -O1 and -O2:
//
//   b = "ABCDEFGH"
//   copy_bytes(a, b, 8)      -> a = "ABCDEFGH"
//   copy_bytes(b + 2, b, 6)  -> b = "ABABABAB"   (d > s: bytes repeat)
//   copy_bytes(b, b + 2, 6)  -> b = "CDEFGHGH"   (d < s: the source is read before it is written)
//
//     ceresc examples/33_loop_copy.c --run

void put(char c)
{
    volatile unsigned int* terminal = (volatile unsigned int*)0xFF000004;
    *terminal = c;
}

char a[16];
char b[16];

void copy_bytes(char* d, char* s, int n)
{
    for (int i = 0; i < n; i = i + 1)
        d[i] = s[i];
}

void print(char* p, int n)
{
    for (int i = 0; i < n; i = i + 1)
        put(p[i]);
    put('\n');
}

int main(void)
{
    for (int i = 0; i < 8; i = i + 1)
        b[i] = 'A' + i;

    copy_bytes(a, b, 8);
    print(a, 8); // ABCDEFGH

    for (int i = 0; i < 8; i = i + 1)
        b[i] = 'A' + i;
    copy_bytes(b + 2, b, 6);
    print(b, 8); // ABABABAB

    for (int i = 0; i < 8; i = i + 1)
        b[i] = 'A' + i;
    copy_bytes(b, b + 2, 6);
    print(b, 8); // CDEFGHGH

    return 0;
}

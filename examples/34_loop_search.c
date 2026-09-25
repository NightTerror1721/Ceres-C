// 34 - byte-scan loops lowered to the compiler's own word-at-a-time routines.
//
// `length` is a hand-written `while (s[i] != 0) i++;` and `find` a hand-written `for (i = 0; i < n;
// i++) if (s[i] == c) break;`. Loop-idiom recognition (docs/14 O15) replaces them with calls to
// `__cc_strlen` and `__cc_memchr_index`, routines the compiler emits into the same unit (never
// linked from the library) that scan a word at a time with the zero-byte test
// `(w - 0x01010101) & ~w & 0x80808080`. The counter is the result, so each routine's return value
// is stored into it before the loop is left.
//
// The scanned string lives in `.rodata`, so the routines also exercise their unaligned prologue.
// The printed values are computed by hand and are the same at -O0, -O1 and -O2:
//
//   text = "abcdefghij"
//   length(text)        = 10
//   length(text + 3)    = 7
//   find(text, 10, 'a') = 0
//   find(text, 10, 'g') = 6
//   find(text, 10, 'z') = 10   (not found: the bound n)
//   find(text,  4, 'g') = 4    (not found within the first four bytes)
//   find(text,  0, 'a') = 0    (a zero bound)
//   find(text, -1, 'a') = 0    (a negative bound)
//
//     ceresc examples/34_loop_search.c --run

void put(char c)
{
    volatile unsigned int* terminal = (volatile unsigned int*)0xFF000004;
    *terminal = c;
}

void putint(int value)
{
    if (value < 0)
    {
        put('-');
        value = -value;
    }
    if (value >= 10)
        putint(value / 10);
    put('0' + value % 10);
}

int length(char* s)
{
    int i = 0;
    while (s[i] != 0)
    {
        i = i + 1;
    }
    return i;
}

int find(char* s, int n, char c)
{
    int i;
    for (i = 0; i < n; i = i + 1)
    {
        if (s[i] == c)
            break;
    }
    return i;
}

int main(void)
{
    char* text = "abcdefghij";

    putint(length(text));
    put('\n');
    putint(length(text + 3));
    put('\n');

    putint(find(text, 10, 'a'));
    put('\n');
    putint(find(text, 10, 'g'));
    put('\n');
    putint(find(text, 10, 'z'));
    put('\n');
    putint(find(text, 4, 'g'));
    put('\n');
    putint(find(text, 0, 'a')); // a zero bound: nothing is searched
    put('\n');
    putint(find(text, -1, 'a')); // a negative bound: the same
    put('\n');

    return 0;
}

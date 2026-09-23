// 32 - a byte-fill loop lowered to the compiler's own word-at-a-time routine.
//
// `fill` is the canonical `for (i = 0; i < n; i++) p[i] = c;`. On this machine a byte store costs
// the same as a word store, so the loop is four times the work it needs to be; loop-idiom
// recognition (docs/14 O15) replaces it with a call to `__cc_memset`, a routine the compiler emits
// into the same unit (never linked from the library) that fills a word at a time once the
// destination is aligned. The output below is the same at -O0, -O1 and -O2, which is the point:
// the optimization changes the cost, not the meaning.
//
// `fill(buffer, 0, 'X')` is the boundary the routine has to get right - a signed count of zero
// fills nothing, so buffer[0] keeps its previous byte.
//
//   fill(buffer, 5, 'A')  -> buffer = "AAAAA"
//   fill(buffer, 3, -1)   -> buffer[0..2] = 0xFF, so buffer[0] is -1 as a signed char
//   fill(buffer, 0, 'X')  -> nothing, so buffer[0] is still -1
//
//     ceresc examples/32_loop_fill.c --run

void put(char c)
{
    char* terminal = (char*)0xFF000004;
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

char buffer[8];

void fill(char* p, int n, char c)
{
    for (int i = 0; i < n; i = i + 1)
        p[i] = c;
}

int main(void)
{
    fill(buffer, 5, 'A');
    for (int i = 0; i < 5; i = i + 1)
        put(buffer[i]);
    put('\n');

    fill(buffer, 3, -1);
    putint(buffer[0]); // 0xFF read as a signed char is -1
    put('\n');

    fill(buffer, 0, 'X');
    putint(buffer[0]); // still -1: a count of zero filled nothing
    put('\n');

    return 0;
}

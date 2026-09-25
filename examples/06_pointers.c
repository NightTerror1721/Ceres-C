// 06 - taking addresses, dereferencing, and pointer arithmetic that scales.
//
// Every local in Ceres-C lives in a frame slot by default, so `&x` is just the address of that
// slot - one `la` instruction. What a pointer costs is not the address but what knowing it
// forbids: once a local's address escapes into a call, nothing about it may be kept in a register
// across that call, because the callee can write through the pointer. The optimizer is allowed to
// see through a pointer that never escapes, and required not to when it does.
//
//     ceresc examples/06_pointers.c -O0 --run     # every access goes to memory
//     ceresc examples/06_pointers.c -O2 --run     # same answers, far fewer instructions

void put(char c)
{
    volatile unsigned int* terminal = (volatile unsigned int*)0xFF000004;
    *terminal = c;
}

void putstr(char* text)
{
    for (int i = 0; text[i] != 0; i++)
        put(text[i]);
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

// The C way of returning a second result: the caller hands over somewhere to put it.
void divmod(int numerator, int denominator, int* quotient, int* remainder)
{
    *quotient = numerator / denominator;
    *remainder = numerator % denominator;
}

void swap(int* a, int* b)
{
    int temp = *a;
    *a = *b;
    *b = temp;
}

int sumThrough(int* values, int count)
{
    int total = 0;
    for (int* p = values; p < values + count; p++)
        total = total + *p;
    return total;
}

int main(void)
{
    int x = 10;
    int* p = &x;

    putstr("through a pointer:   ");
    *p = *p + 5;
    putint(x);
    put('\n');

    putstr("two results at once: ");
    int quotient = 0;
    int remainder = 0;
    divmod(47, 5, &quotient, &remainder);
    putint(quotient);
    putstr(" remainder ");
    putint(remainder);
    put('\n');

    putstr("swap:                ");
    int a = 1;
    int b = 2;
    swap(&a, &b);
    putint(a);
    put(' ');
    putint(b);
    put('\n');

    // Pointer arithmetic counts in ELEMENTS, not bytes: `values + 2` advances eight bytes for an
    // int array, and subtracting two pointers divides the byte distance back down again.
    int values[5] = { 10, 20, 30, 40, 50 };
    int* first = values;
    int* third = values + 2;
    putstr("scaled arithmetic:   ");
    putint(*third);
    putstr(" is ");
    putint((int)(third - first));
    putstr(" elements along\n");

    putstr("walking with a ptr:  ");
    putint(sumThrough(values, 5));
    put('\n');

    // A pointer into the middle of an array behaves exactly like an array of its own.
    putstr("tail as an array:    ");
    int* tail = &values[2];
    for (int i = 0; i < 3; i++)
    {
        putint(tail[i]);
        put(' ');
    }
    put('\n');

    // char* and int* scale differently over the same bytes - the cast is where the reinterpreting
    // happens, and this machine is little-endian, so byte 0 is the least significant one.
    int packed = 0x04030201;
    char* bytes = (char*)&packed;
    putstr("little-endian bytes: ");
    for (int i = 0; i < 4; i++)
    {
        putint(bytes[i]);
        put(' ');
    }
    put('\n');

    return 0;
}

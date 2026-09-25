// The implementation side of io.h. It includes its own header so the compiler checks the
// declarations against the definitions - the one habit that makes headers worth having.

#include "io.h"

int writeCount = 0;

// `static` gives this internal linkage: it is not published to the linker, so another unit cannot
// call it and unused-function elimination is allowed to drop it if nothing here does either.
static void writeByte(char c)
{
    volatile unsigned int* terminal = (volatile unsigned int*)TERMINAL_OUT;
    *terminal = c;
}

void put(char c)
{
    writeByte(c);
    writeCount++;
}

void putstr(const char* text)
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

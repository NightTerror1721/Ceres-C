// 10 - the integer types, signedness and promotion.
//
// Sizes follow the machine: char 1, short 2, int/long/float/pointer 4. There is no `double` - the
// VM has no double-precision support at all, so it is left out rather than emulated in software.
//
// Signedness is not decoration. It picks the instruction: `/` becomes `idiv` or `div`, `>>`
// becomes `sar` or `shr`, and `<` becomes `ifls` or `ifbl`. The same bits compare differently
// depending on the type the expression has.
//
// Narrow SIGNED values (a negative `signed char` or `short` held in memory) and narrowing casts
// are a known gap in this version - see docs/06-Known-Limitations.md. Everything below stays on
// the side of that line which works, so this file is a description of the language as it is.
//
//     ceresc examples/10_types.c --run

void put(char c)
{
    char* terminal = (char*)0xFF000004;
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

// Unsigned values above 2^31 do not fit in an int, so they get their own printer - and it needs
// no sign branch, which is the whole difference.
void putuint(unsigned int value)
{
    if (value >= 10)
        putuint(value / 10);
    put('0' + (int)(value % 10));
}

int main(void)
{
    putstr("sizes:      char ");
    putint((int)sizeof(char));
    putstr(", short ");
    putint((int)sizeof(short));
    putstr(", int ");
    putint((int)sizeof(int));
    putstr(", long ");
    putint((int)sizeof(long));
    putstr(", float ");
    putint((int)sizeof(float));
    putstr(", int* ");
    putint((int)sizeof(int*));
    put('\n');

    // signed/unsigned/short/long combine with int and char just as they do in C.
    unsigned char byte = 200;
    unsigned short halfword = 60000;
    long wide = 2000000000;
    unsigned int uwide = 4000000000;

    putstr("ranges:     ");
    putint(byte);
    put(' ');
    putint(halfword);
    put(' ');
    putint((int)wide);
    put(' ');
    putuint(uwide);
    put('\n');

    // Same 32 bits, two readings. As an int it is -1; as an unsigned int it is 4294967295.
    unsigned int allOnes = 0xFFFFFFFF;
    putstr("same bits:  signed ");
    putint((int)allOnes);
    putstr(", unsigned ");
    putuint(allOnes);
    put('\n');

    // ...and therefore they compare differently: `ifls` against `ifbl`.
    putstr("comparison: ");
    if ((int)allOnes < 1)
        putstr("as int, -1 < 1");
    putstr("; ");
    if (allOnes > 1)
        putstr("as unsigned, 4294967295 > 1");
    put('\n');

    // char and short are promoted to int before any arithmetic happens, so this addition is done
    // in 32 bits and does not wrap at 8.
    putstr("promotion:  ");
    unsigned char a = 100;
    unsigned char b = 200;
    putint(a + b);
    putstr(" - the sum is an int, not a char\n");

    // Signed and unsigned division are different instructions, and so are the shifts.
    putstr("division:   ");
    putint(-7 / 2);
    putstr(" signed (truncates toward zero), ");
    putuint(uwide / 1000000);
    putstr(" unsigned (no idiv could do this)\n");

    putstr("shifts:     ");
    putint(-32 >> 2);
    putstr(" arithmetic (sar), ");
    putuint(allOnes >> 28);
    putstr(" logical (shr)\n");

    // bool is its own type, with `true` and `false` as literals.
    bool yes = true;
    bool no = false;
    putstr("bool:       ");
    putint(yes);
    put(' ');
    putint(no);
    putstr(", sizeof ");
    putint((int)sizeof(bool));
    put('\n');

    // A char literal is an integer: 'A' is 65, and arithmetic on it is ordinary int arithmetic.
    putstr("chars:      ");
    put('A');
    putstr(" is ");
    putint('A');
    putstr(", and 'A' + 2 is ");
    put('A' + 2);
    put('\n');

    // sizeof takes a type name or an expression, and never evaluates the expression.
    putstr("sizeof:     ");
    putint((int)sizeof(int));
    putstr(" for a type, ");
    putint((int)sizeof(wide + 1));
    putstr(" for an expression\n");

    return 0;
}

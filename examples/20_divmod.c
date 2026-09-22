// 20 - division and remainder by a power of two.
//
// A division or remainder is the expensive half of an arithmetic instruction set, and dividing by
// a constant power of two is one shift or mask - which is what strength reduction turns it into
// (docs/14, O1). The catch is signed arithmetic: C truncates toward zero, while a bare arithmetic
// shift rounds toward minus infinity, so the optimizer biases the dividend first. This program is
// the end-to-end check that the shifted form agrees with the machine's own idiv/imod on negative
// dividends and on the extremes, and that the unsigned forms keep their logical meaning.
//
// The divisors are literals on purpose: a divisor read from an array would be a run-time value and
// would keep its `idiv`. Every line must print the same bytes at -O0, -O1 and -O2.
//
//     ceresc examples/20_divmod.c --run

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

void putuint(unsigned int value)
{
    if (value >= 10)
        putuint(value / 10);
    put('0' + value % 10);
}

// One signed value, divided and reduced by 2, 4, 8 and 16. The divisors are compile-time constants
// here, which is the whole point: `v / 2` becomes a shift, `v % 2` becomes a bias-and-mask.
void signedLine(int v)
{
    putint(v);
    put(':');

    put(' ');
    putint(v / 2); put(','); putint(v % 2);
    put(' ');
    putint(v / 4); put(','); putint(v % 4);
    put(' ');
    putint(v / 8); put(','); putint(v % 8);
    put(' ');
    putint(v / 16); put(','); putint(v % 16);
    put('\n');
}

void unsignedLine(unsigned int v)
{
    putuint(v);
    put(':');

    put(' ');
    putuint(v / 2u); put(','); putuint(v % 2u);
    put(' ');
    putuint(v / 16u); put(','); putuint(v % 16u);
    put(' ');
    putuint(v / 256u); put(','); putuint(v % 256u);
    put('\n');
}

int main(void)
{
    putstr("signed: dividend: quotient,remainder\n");
    signedLine(0);
    signedLine(1);
    signedLine(-1);
    signedLine(7);
    signedLine(-7);
    signedLine(16);
    signedLine(-16);
    signedLine(100);
    signedLine(-100);

    putstr("unsigned:\n");
    unsignedLine(0u);
    unsignedLine(1u);
    unsignedLine(255u);
    unsignedLine(4000000000u);

    // The extreme dividend, tested rather than printed: INT_MIN / 4 and INT_MIN % 4 are exact, and
    // INT_MIN is the one value whose negation does not fit a signed int.
    int smallest = -2147483647 - 1;
    putstr("INT_MIN / 4 == -536870912: ");
    putint((smallest / 4 == -536870912) ? 1 : 0);
    putstr("\nINT_MIN % 4 == 0: ");
    putint((smallest % 4 == 0) ? 1 : 0);
    put('\n');

    return 0;
}

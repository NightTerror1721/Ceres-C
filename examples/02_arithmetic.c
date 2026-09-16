// 02 - integer arithmetic, precedence and the two flavours of division.
//
// Every operator here maps to a single CeresASM instruction (docs/03-IR-to-CASM.md). The ones
// worth looking at in the generated .casm are `/` and `>>`: signed operands assemble to `idiv`
// and `sar`, unsigned ones to `div` and `shr`. That distinction is real in the ISA, not cosmetic.
//
//     ceresc examples/02_arithmetic.c --run

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

// Recursive rather than looping: the digits come out most-significant first, and recursion is the
// shortest way to reverse them without a scratch buffer.
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

void line(char* label, int value)
{
    putstr(label);
    putint(value);
    put('\n');
}

int main(void)
{
    int a = 17;
    int b = 5;

    line("a + b      = ", a + b);
    line("a - b      = ", a - b);
    line("a * b      = ", a * b);
    line("a / b      = ", a / b);
    line("a % b      = ", a % b);

    // C truncates towards zero, so a negative quotient loses the fraction rather than rounding
    // down, and the remainder keeps the sign of the dividend.
    line("-a / b     = ", -a / b);
    line("-a % b     = ", -a % b);

    // Precedence, not evaluation order: `*` binds tighter than `+`, and the parentheses change
    // the tree the parser builds (docs/02-Grammar.md).
    line("a + b * 2  = ", a + b * 2);
    line("(a + b) * 2= ", (a + b) * 2);

    // A signed right shift keeps the sign bit (`sar`); the unsigned one does not (`shr`).
    int negative = -32;
    unsigned int wide = 0xFFFFFFF0;
    line("-32 >> 2   = ", negative >> 2);
    line("0xFFFFFFF0 >> 28 (unsigned) = ", (int)(wide >> 28));

    return 0;
}

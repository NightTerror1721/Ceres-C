// 14 - the bitwise operators, and the kind of code they exist for.
//
// Each one is a single instruction (`and`, `or`, `xor`, `not`, `shl`, `sar`/`shr`), and the
// peephole pass hands a constant right-hand side straight to the instruction's immediate form -
// so `flags | 4` is one instruction, not a load of 4 followed by an or.
//
//     ceresc examples/14_bitwise.c -O0 -o 14-plain.casm
//     ceresc examples/14_bitwise.c -O2 -o 14-opt.casm     # diff the two
//     ceresc examples/14_bitwise.c --run

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

// Prints the low `width` bits, most significant first.
void putbits(unsigned int value, int width)
{
    for (int i = width - 1; i >= 0; i--)
        put('0' + (int)((value >> i) & 1));
}

void putHex(unsigned int value)
{
    char* digits = "0123456789abcdef";
    put('0');
    put('x');
    for (int i = 7; i >= 0; i--)
        put(digits[(value >> (i * 4)) & 15]);
}

int countOneBits(unsigned int value)
{
    int count = 0;
    while (value != 0)
    {
        count = count + (int)(value & 1);
        value = value >> 1;
    }
    return count;
}

// Clears the lowest set bit each time round, so it loops once per set bit instead of 32 times.
int countOneBitsFast(unsigned int value)
{
    int count = 0;
    while (value != 0)
    {
        value = value & (value - 1);
        count++;
    }
    return count;
}

unsigned int reverseBits(unsigned int value, int width)
{
    unsigned int result = 0;
    for (int i = 0; i < width; i++)
    {
        result = (result << 1) | (value & 1);
        value = value >> 1;
    }
    return result;
}

bool isPowerOfTwo(unsigned int value)
{
    if (value == 0)
        return false;
    return (value & (value - 1)) == 0;
}

int main(void)
{
    unsigned int a = 0xF0;
    unsigned int b = 0x3C;

    putstr("a        = ");
    putbits(a, 8);
    putstr("  ");
    putHex(a);
    put('\n');

    putstr("b        = ");
    putbits(b, 8);
    putstr("  ");
    putHex(b);
    put('\n');

    putstr("a & b    = ");
    putbits(a & b, 8);
    put('\n');

    putstr("a | b    = ");
    putbits(a | b, 8);
    put('\n');

    putstr("a ^ b    = ");
    putbits(a ^ b, 8);
    put('\n');

    putstr("~a       = ");
    putbits(~a, 8);
    putstr("  (low 8 bits of ");
    putHex(~a);
    putstr(")\n");

    putstr("a << 1   = ");
    putbits(a << 1, 9);
    put('\n');

    putstr("a >> 4   = ");
    putbits(a >> 4, 8);
    put('\n');

    // Setting, clearing and testing one bit - what the operators are actually for.
    putstr("bit ops  = ");
    unsigned int flags = 0;
    flags = flags | (1 << 3);
    flags = flags | (1 << 5);
    putbits(flags, 8);
    putstr(" set, ");
    flags = flags & ~(1 << 3);
    putbits(flags, 8);
    putstr(" cleared, bit 5 is ");
    putint((int)((flags >> 5) & 1));
    put('\n');

    putstr("popcount = ");
    for (int i = 0; i < 8; i++)
    {
        putint(countOneBits(i));
        put(' ');
    }
    putstr("| fast: ");
    putint(countOneBitsFast(0xFFFF));
    put('\n');

    putstr("reversed = ");
    putbits(reverseBits(0xB, 8), 8);
    putstr(" from ");
    putbits(0xB, 8);
    put('\n');

    putstr("powers   = ");
    for (int i = 0; i <= 9; i++)
    {
        putint(isPowerOfTwo(i));
        put(' ');
    }
    put('\n');

    // Multiplying and dividing by a power of two, spelled as shifts.
    putstr("shifts   = ");
    putint(7 << 3);
    putstr(" is 7*8, ");
    putint(56 >> 3);
    putstr(" is 56/8\n");

    return 0;
}

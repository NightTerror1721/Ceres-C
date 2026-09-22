// 21 - the one-instruction builtins.
//
// A handful of machine instructions have no C expression: clz, ctz, popcount, bswap, rotate, the
// multiply-high, abs, and the float operations that are not an operator (sqrt, floor, min, the
// reciprocal estimates, and the raw bit moves between an int and a float). Each is spelled
// `__builtin_<name>(...)` and lowers to exactly one instruction - the same instructions the Ceres
// STDLIB reaches today through hand-written CASM (asm/bits.casm, asm/math_ops.casm).
//
// Every expected value here is computed by hand, not by running this program, so the test is a
// check on the instructions rather than on themselves.
//
//     ceresc examples/21_builtins.c --run

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

// Always eight lowercase hex digits after "0x", so the expected file is easy to read.
void puthex32(unsigned int value)
{
    putstr("0x");
    for (int shift = 28; shift >= 0; shift -= 4)
    {
        int digit = (value >> shift) & 15;
        put(digit < 10 ? '0' + digit : 'a' + (digit - 10));
    }
}

void line(char* label, int value)
{
    putstr(label);
    putint(value);
    put('\n');
}

void hexline(char* label, unsigned int value)
{
    putstr(label);
    puthex32(value);
    put('\n');
}

int main(void)
{
    // The bit instructions.
    line("clz(1)          = ", (int)__builtin_clz(1u));            // 31
    line("ctz(8)          = ", (int)__builtin_ctz(8u));            // 3
    line("popcount(f0f0)  = ", (int)__builtin_popcount(0xF0F0u));  // 8
    hexline("bswap(11223344) = ", __builtin_bswap32(0x11223344u)); // 0x44332211
    hexline("rotl(80000001,1)= ", __builtin_rotl32(0x80000001u, 1u)); // 0x00000003
    hexline("rotr(00000003,1)= ", __builtin_rotr32(0x00000003u, 1u)); // 0x80000001
    line("abs(-5)         = ", __builtin_abs(-5));                 // 5
    // The high half of (2^32-1)^2 = 2^64 - 2^33 + 1 is 2^32 - 2.
    hexline("mulhu(ffffffff) = ", __builtin_mulhu(0xFFFFFFFFu, 0xFFFFFFFFu)); // 0xfffffffe
    // The signed high half: (2^30)^2 = 2^60, whose high word is 2^28 = 268435456.
    line("mulhs(2^30,2^30) = ", __builtin_mulhs(0x40000000, 0x40000000));      // 268435456

    // The float instructions, checked through an int so no float printer is needed.
    line("sqrt(16)        = ", (int)__builtin_sqrt(16.0f));       // 4
    line("floor(3.9)      = ", (int)__builtin_floor(3.9f));       // 3
    line("ceil(3.1)       = ", (int)__builtin_ceil(3.1f));        // 4
    line("trunc(-3.9)     = ", (int)__builtin_trunc(-3.9f));      // -3
    line("rint(3.5)       = ", (int)__builtin_rint(3.5f));        // 4 (ties to even)
    line("rint(2.5)       = ", (int)__builtin_rint(2.5f));        // 2 (ties to even)
    line("fmod(7.5,2)     = ", (int)__builtin_fmod(7.5f, 2.0f));  // 1 (7.5 - 3*2)
    line("fabs(-3)        = ", (int)__builtin_fabs(-3.0f));       // 3
    line("fmin(2,5)       = ", (int)__builtin_fmin(2.0f, 5.0f));  // 2
    line("fmax(2,5)       = ", (int)__builtin_fmax(2.0f, 5.0f));  // 5
    line("imin(-3,5)      = ", __builtin_imin(-3, 5));            // -3
    line("imax(-3,5)      = ", __builtin_imax(-3, 5));            // 5

    // The raw bit moves: 1.0f is 0x3F800000, and 0x40000000 is 2.0f.
    hexline("bits(1.0)       = ", __builtin_float_bits(1.0f));    // 0x3f800000
    line("from_bits(4000) = ", (int)__builtin_float_from_bits(0x40000000u)); // 2

    // Classification bits: -1.0f is bit 1, +1.0f is bit 6.
    line("fclass(-1.0)    = ", __builtin_fclass(-1.0f));          // 2
    line("fclass(1.0)     = ", __builtin_fclass(1.0f));           // 64

    return 0;
}

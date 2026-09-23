// 35 - real 64-bit integers.
//
// `long long` and `unsigned long long` are 8-byte types, not spellings of the 32-bit ones:
// `sizeof`, struct layout and the `ll`/`LL` literal suffix all see the full width. Their values
// lower as an addressed pair of words (low at +0, high at +4), so arithmetic, the bitwise operators
// and comparisons compute on all 64 bits, and a value that does not fit 32 bits keeps its high half
// through a variable, a struct field, an array element and a global.
//
// The two halves are read back through a union because this target is little-endian; the helper
// takes a POINTER to the value rather than the value itself, because a 64-bit value cannot yet cross
// a function boundary (the wide calling convention is a later phase).
//
// Shifts, and the conversions between a 64-bit integer and a `float`, are exercised below too:
// a shift by 32 or more crosses the word boundary, and a float conversion goes sixteen bits at a
// time (a value that does not fit 32 bits keeps its high half through the float and back).
//
// A 64-bit value now crosses a function boundary by value: an argument travels in two consecutive
// parameter registers (or two outgoing stack words, once the four are spent) and a result comes back
// in ret0/ret1. `widen`, `add3` and `sum6` below exercise the register and stack paths both ways.
//
//     ceresc examples/35_int64.c --run

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

// One 64-bit value, printed as its two 32-bit words. The parameter is a pointer because the 64-bit
// calling convention is a later phase, so the value is reached by pointer and never passed by value.
// Reading the inactive
// union member is the little-endian assumption the whole lowering makes (low word first), so this
// only prints `halves[0]` as the low word because Ceres stores the low word at offset 0.
void put64(char* label, long long* value)
{
    union
    {
        long long whole;
        int halves[2];
    } split;
    split.whole = *value;

    putstr(label);
    putstr(" low=");
    putint(split.halves[0]);
    putstr(" high=");
    putint(split.halves[1]);
    put('\n');
}

// A file-scope 64-bit object: codegen writes it as `u32[2] = [low, high]`, and reading it back goes
// through the same two-word load as a local.
long long globalWide = 0x0000000A0000000BLL;

// Like put64(), but taking the 64-bit value BY VALUE (F3.4): its two words arrive in parameter
// registers, and the union's `whole` member and this `value` are the same eight bytes.
void putvalue(char* label, long long value)
{
    union
    {
        long long whole;
        int halves[2];
    } split;
    split.whole = value;

    putstr(label);
    putstr(" low=");
    putint(split.halves[0]);
    putstr(" high=");
    putint(split.halves[1]);
    put('\n');
}

// The 64-bit calling convention (F3.4), defined after `main` below.
long long widen(long long value);
long long add3(long long a, long long b, long long c);
long long sum6(long long a, long long b, long long c, long long d, long long e, long long f);

int main(void)
{
    // hi = 1, lo = 2: a value that does not fit 32 bits.
    long long a = 0x0000000100000002LL;
    long long b = 1;

    put64("a", &a);
    put64("b", &b);

    long long sum = a + b;
    long long difference = a - b;
    long long negated = -a;
    long long anded = a & b;
    long long ored = a | b;
    long long xored = a ^ b;
    long long inverted = ~a;
    put64("a + b", &sum);
    put64("a - b", &difference);
    put64("-a", &negated);
    put64("a & b", &anded);
    put64("a | b", &ored);
    put64("a ^ b", &xored);
    put64("~a", &inverted);
    put64("globalWide", &globalWide);

    // The carry and borrow that cross the word boundary: a low word of zero is exactly where a
    // negation has to carry into the high word, and `b - a` borrows out of it.
    long long power = 0x0000000100000000LL; // lo = 0, hi = 1
    long long negatedPower = -power;
    long long borrowed = b - a;
    put64("0x100000000", &power);
    put64("-0x100000000", &negatedPower);
    put64("b - a", &borrowed);

    // Multiplication composes from the 32-bit multiply and multiply-high; division and remainder
    // call the compiler's own `__cc_div64`, a restoring shift-subtract loop. Signed division
    // truncates toward zero and the remainder takes the dividend's sign, as C requires.
    long long product = a * b;
    long long bigProduct = 0x0000000100000002LL * 0x0000000100000002LL;
    long long quotient = a / 5;
    long long remainder = a % 5;
    long long negatedQuotient = -a / 5;
    long long negatedRemainder = -a % 5;
    long long unsignedQuotient = (unsigned long long)a / 3ULL;
    put64("a * b", &product);
    put64("big * big", &bigProduct);
    put64("a / 5", &quotient);
    put64("a % 5", &remainder);
    put64("-a / 5", &negatedQuotient);
    put64("-a % 5", &negatedRemainder);
    put64("(unsigned)a / 3", &unsignedQuotient);

    // Shifts. The three arms (0, 1..31, 32..63) are all hit, and the operands have their top bits
    // set so the cross-word term is not lost: `<< 4` takes bits out of the low word's top, `>> 4`
    // brings bits down from the high word, and a wide `>>` keeps the sign (arithmetic) or not.
    long long shiftedLeftSmall = 0x00000001F0000002LL << 4;
    long long shiftedLeftLarge = a << 40;
    long long shiftedLeftZero = a << 0;
    long long shiftedRightSmall = 0x0000000100000002LL >> 4;
    long long shiftedRightLarge = (-1LL) >> 36;          // arithmetic: sign-fills the high word
    long long unsignedRightLarge = (unsigned long long)(-1LL) >> 36; // logical: zero-fills
    long long negativeRightSmall = (-1LL) >> 1;          // arithmetic: stays -1
    put64("0x1F0000002 << 4", &shiftedLeftSmall);
    put64("a << 40", &shiftedLeftLarge);
    put64("a << 0", &shiftedLeftZero);
    put64("0x100000002 >> 4", &shiftedRightSmall);
    put64("-1 >> 36", &shiftedRightLarge);
    put64("(unsigned)-1 >> 36", &unsignedRightLarge);
    put64("-1 >> 1", &negativeRightSmall);

    // A 64-bit value compared against a float converts the whole value first. `a` is 0x100000002
    // (4294967298), which rounds to 4294967296.0f; `4294967297.0f` rounds to the same, so `==` holds
    // (a float cannot tell them apart), while `a > 1.0f` is plainly true.
    putstr("a == 4294967297.0f: ");
    put(a == 4294967297.0f ? '1' : '0');
    put('\n');
    putstr("a > 1.0f: ");
    put(a > 1.0f ? '1' : '0');
    put('\n');
    putstr("a < 1.0f: ");
    put(a < 1.0f ? '1' : '0');
    put('\n');

    // float <-> 64-bit. A value with a non-zero high half survives the round trip through a float
    // (f32 holds 24 significant bits, so 0x100000002 rounds to 0x100000000 - the high word is what
    // is checked). The exact cases below are small enough to be represented precisely.
    float fromWide = (float)0x0000000100000002LL; // 4294967298 -> 4294967296.0f (rounds)
    long long backToWide = (long long)4294967296.0f;
    long long negativeToWide = (long long)(-1.0f);
    putstr("(float)0x100000002 == 4294967296.0f: ");
    put(fromWide == 4294967296.0f ? '1' : '0');
    put('\n');
    put64("(long long)4294967296.0f", &backToWide);
    put64("(long long)-1.0f", &negativeToWide);

    // The high word carries the comparison: signed and unsigned disagree on `-1`.
    putstr("a < a + b: ");
    put(a < a + b ? '1' : '0');
    put('\n');
    putstr("a == 0x100000002: ");
    put(a == 0x0000000100000002LL ? '1' : '0');
    put('\n');
    putstr("(unsigned)(-1) > 1: ");
    put((unsigned long long)(-1) > 1ULL ? '1' : '0');
    put('\n');
    putstr("(long long)(-1) < 1: ");
    put((long long)(-1) < 1LL ? '1' : '0');
    put('\n');

    // The high half survives a struct field and an array element too.
    struct Pair
    {
        int tag; // only here so `value` lands at offset 8 - the wide field's real address
        long long value;
    };
    struct Pair pair;
    pair.value = a + b;
    put64("pair.value", &pair.value);

    long long table[3];
    table[0] = a;
    table[1] = b;
    table[2] = table[0] + table[1];
    put64("table[2]", &table[2]);

    // A post-increment's value is the OLD one, and a `bool` sees the whole 64-bit value (the low
    // word alone would call 0x100000000 false).
    long long counter = a;
    long long beforeIncrement = counter++;
    put64("counter after ++", &counter);        // a + 1 -> 3,1
    put64("counter++ value", &beforeIncrement); // a -> 2,1
    putstr("(bool)0x100000000: ");
    put((bool)0x100000000LL ? '1' : '0');
    put('\n');

    // One 64-bit value, printed as its two 32-bit words. A parameter arrives in two registers, so
    // the union's `whole` member and this helper's `value` are the same eight bytes.
    putvalue("widen(0x100000000)", widen(0x0000000100000000LL));

    // Two wide arguments fill r0-r3 exactly; the third wide argument goes to two outgoing stack
    // words, and the result comes back in ret0/ret1.
    putvalue("add3(1, 2, 3)", add3(1LL, 2LL, 3LL));
    putvalue("add3(0x100000000, 2, 3)", add3(0x0000000100000000LL, 2LL, 3LL));

    // Six wide arguments: the first two take registers, the rest the outgoing stack.
    putvalue("sum6(1..6)", sum6(1LL, 2LL, 3LL, 4LL, 5LL, 6LL));

    return 0;
}

// A 64-bit parameter arrives in two consecutive argument registers (r0/r1 for the first), and a
// 64-bit result goes back in ret0/ret1. `widen` adds a constant whose low word is zero, so the
// addition itself has to carry into the high word.
long long widen(long long value)
{
    return value + 0x0000000100000000LL;
}

// Three wide parameters: the first two fill all four int argument registers, and the third is passed
// on the outgoing stack as two words - and comes back as two words in ret0/ret1.
long long add3(long long a, long long b, long long c)
{
    return a + b + c;
}

// Six wide parameters: every argument is on the outgoing stack, read back two words apiece.
long long sum6(long long a, long long b, long long c, long long d, long long e, long long f)
{
    return a + b + c + d + e + f;
}

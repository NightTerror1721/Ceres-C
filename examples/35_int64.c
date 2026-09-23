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
// Shifts and the float conversions are the next phase and are refused with E5002 rather than
// silently truncated; every line here is add/sub/mul/div/mod/bitwise/compare.
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

    return 0;
}

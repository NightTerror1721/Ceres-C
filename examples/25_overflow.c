// 25 - overflow-checked arithmetic: `__builtin_<op>_overflow(a, b, &result)`.
//
// Each stores `a op b` into `*result` (as the machine's 32-bit wrap-around always did) and returns
// whether the operation overflowed. Everything here is computed by hand, so the expected file is a
// check on the builtins rather than on themselves.
//
//     ceresc examples/25_overflow.c --run

void put(char c)
{
    volatile unsigned int* terminal = (volatile unsigned int*)0xFF000004;
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

void putuint(unsigned int value)
{
    if (value >= 10)
        putuint(value / 10);
    put('0' + value % 10);
}

int main(void)
{
    int r;
    unsigned int ur;
    int overflowed;

    overflowed = __builtin_add_overflow(1, 2, &r);           // 0, r = 3
    putint(overflowed); put(' '); putint(r); put('\n');

    overflowed = __builtin_add_overflow(2147483647, 1, &r);  // 1 (INT_MAX + 1)
    putint(overflowed); put('\n');

    overflowed = __builtin_sub_overflow(5, 3, &r);           // 0, r = 2
    putint(overflowed); put(' '); putint(r); put('\n');

    overflowed = __builtin_sub_overflow(-2147483647, 2, &r); // 1 (below INT_MIN), r wraps to 2147483647
    putint(overflowed); put(' '); putint(r); put('\n');

    overflowed = __builtin_mul_overflow(100000, 100000, &r); // 1 (10^10), r = 10^10 mod 2^32
    putint(overflowed); put(' '); putint(r); put('\n');

    overflowed = __builtin_mul_overflow(3, 4, &r);           // 0, r = 12
    putint(overflowed); put(' '); putint(r); put('\n');

    overflowed = __builtin_add_overflow(4000000000u, 1000000000u, &ur); // 1, ur = 705032704
    putint(overflowed); put(' '); putuint(ur); put('\n');

    overflowed = __builtin_mul_overflow(3u, 4u, &ur);        // 0, ur = 12
    putint(overflowed); put(' '); putuint(ur); put('\n');

    return 0;
}

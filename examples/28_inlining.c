// 28 - inlining: small functions become part of their callers.
//
// At -O2 the compiler splices a small callee's body straight into the call site, so a call to a
// helper costs nothing (no `call`/`ret`, no argument shuffle). This now covers callees with control
// flow and callees that make calls of their own - the shapes a real program is made of. Only the
// result is observable here, but `--stats` reports how many calls were inlined, and
// `-fno-inline` turns it off. Expected: 3 0 12 15 55.
//
//     ceresc examples/28_inlining.c --run
//     ceresc examples/28_inlining.c --run -O2 --stats

void put(char c)
{
    char* terminal = (char*)0xFF000004;
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

// Control flow: two branches and three returns, still small enough to splice.
int clamp(int x, int lo, int hi)
{
    if (x < lo)
        return lo;
    if (x > hi)
        return hi;
    return x;
}

// A callee that calls another callee.
int add1(int x)
{
    return x + 1;
}

int twice(int x)
{
    return add1(x) + add1(x);
}

// A loop.
int sumTo(int n)
{
    int s = 0;
    for (int i = 1; i <= n; i = i + 1)
        s = s + i;
    return s;
}

int main(void)
{
    putint(clamp(5, 0, 3)); // 3
    put('\n');
    putint(clamp(-4, 0, 3)); // 0
    put('\n');
    putint(twice(5)); // 12
    put('\n');
    putint(sumTo(5)); // 15
    put('\n');
    putint(sumTo(10)); // 55
    put('\n');
    return 0;
}

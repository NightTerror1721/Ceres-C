// 04 - calls, recursion and the two shapes a function's prologue can take.
//
// Ceres-C follows CeresASM's own calling convention (docs/24-Calling-Convention.md): the first
// four arguments travel in arg0-arg3, the rest on the stack, and the result comes back in ret0.
// A function that needs nothing from a frame - few enough arguments, no locals that have to live
// in memory, nothing held across a call - skips `enter`/`leave` entirely. Compare the two in the
// generated text:
//
//     ceresc examples/04_functions.c -o 04.casm   # `square` has no frame; `sumTo` does
//     ceresc examples/04_functions.c --run

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

// A leaf with no frame: one multiply and a `ret`.
int square(int n)
{
    return n * n;
}

// Recursion. Each call pushes its own frame, so `n` is a different variable at every depth - the
// VM raises StackOverflow if a recursion never bottoms out, which is why Ceres-C does not check
// depth itself.
int factorial(int n)
{
    if (n <= 1)
        return 1;
    return n * factorial(n - 1);
}

// Two recursive calls per level: deliberately the slow definition, because what it demonstrates is
// that a value held ACROSS a call (`fib(n - 1)`'s result, while `fib(n - 2)` runs) gets spilled to
// a frame slot and reloaded - `call` destroys the caller-saved registers.
int fib(int n)
{
    if (n < 2)
        return n;
    return fib(n - 1) + fib(n - 2);
}

// Six parameters: arg0-arg3 arrive in registers, the last two on the stack.
int sum6(int a, int b, int c, int d, int e, int f)
{
    return a + b + c + d + e + f;
}

// A function whose result is discarded, and one that returns nothing at all.
void repeat(char c, int times)
{
    while (times > 0)
    {
        put(c);
        times--;
    }
}

int main(void)
{
    putstr("square(7)   = ");
    putint(square(7));
    put('\n');

    putstr("factorial   = ");
    for (int i = 0; i <= 7; i++)
    {
        putint(factorial(i));
        put(' ');
    }
    put('\n');

    putstr("fib         = ");
    for (int i = 0; i < 10; i++)
    {
        putint(fib(i));
        put(' ');
    }
    put('\n');

    putstr("sum6        = ");
    putint(sum6(1, 2, 3, 4, 5, 6));
    put('\n');

    putstr("nested      = ");
    putint(square(factorial(3)));
    put('\n');

    putstr("repeat      = ");
    repeat('=', 8);
    put('\n');

    return 0;
}

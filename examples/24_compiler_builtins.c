// 24 - the compiler builtins that are not a machine instruction.
//
// `__builtin_constant_p(e)` is a compile-time question (1 when `e` folds to a constant, 0
// otherwise) and does not evaluate `e`; `__builtin_expect(x, hint)` is a branch-prediction hint
// whose value is just `x`. `__builtin_trap()` and `__builtin_unreachable()` both lower to the
// machine's `trap`. Printed: 1, 0, 5.
//
//     ceresc examples/24_compiler_builtins.c --run

void put(char c)
{
    char* terminal = (char*)0xFF000004;
    *terminal = c;
}

void putint(int value)
{
    if (value >= 10)
        putint(value / 10);
    put('0' + value % 10);
}

int main(void)
{
    putint(__builtin_constant_p(3 + 4)); // 1: a literal expression
    put('\n');

    int x = 5;
    putint(__builtin_constant_p(x)); // 0: a variable's value is not a compile-time constant here
    put('\n');

    putint(__builtin_expect(x, 1)); // 5: the hint has no value of its own
    put('\n');

    return 0;
}

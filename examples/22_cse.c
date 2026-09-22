// 22 - repeated pure expressions, and the ternary that must not be confused with them.
//
// `s * s` is computed once and reused (common subexpression elimination, docs/14 O2), while
// `c ? a : b` lowers to a value with TWO definitions on two paths - the shape a non-SSA CSE pass has
// to leave alone. Both appear twice here, so a CSE that merged the wrong thing would show up as a
// wrong number rather than a crash. Expected: 158 then 154.
//
//     ceresc examples/22_cse.c --run

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

int combine(int chooseFirst, int a, int b)
{
    int sum = a + b;
    // `sum * sum` repeated is a pure expression CSE may share; `chooseFirst ? a : b` repeated is a
    // phi-shaped value (one definition per arm), which it must not.
    return (sum * sum) + (chooseFirst ? a : b) + (chooseFirst ? a : b);
}

int main(void)
{
    putint(combine(1, 7, 5)); // 12*12 + 7 + 7 = 158
    put('\n');
    putint(combine(0, 7, 5)); // 12*12 + 5 + 5 = 154
    put('\n');
    return 0;
}

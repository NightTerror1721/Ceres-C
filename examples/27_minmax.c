// 27 - select idioms: `a < b ? a : b` and `x < 0 ? -x : x`.
//
// A min/max/abs written as a ternary is a branch diamond when compiled literally (a compare and
// two arms). The compiler recognizes the shape and emits the one instruction the ISA already has
// (imin/imax, umin/umax, abs), so the same source runs faster without changing what it computes.
// The operands must be the comparison's own (and side-effect free), which is why the two arm
// orders and the signed/unsigned pair are all spelled out here. Expected: 3 3 7 7 4 4 0 1 2.
//
//     ceresc examples/27_minmax.c --run

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

int mn(int a, int b)
{
    return a < b ? a : b;
}

int mx(int a, int b)
{
    return a > b ? a : b;
}

int ab(int a)
{
    return a < 0 ? -a : a;
}

unsigned int umn(unsigned int a, unsigned int b)
{
    return a < b ? a : b;
}

unsigned int umx(unsigned int a, unsigned int b)
{
    return a > b ? a : b;
}

int main(void)
{
    putint(mn(3, 7)); // 3
    put('\n');
    putint(mn(7, 3)); // 3
    put('\n');
    putint(mx(3, 7)); // 7
    put('\n');
    putint(mx(7, 3)); // 7
    put('\n');
    putint(ab(-4)); // 4
    put('\n');
    putint(ab(4)); // 4
    put('\n');
    putint(ab(0)); // 0
    put('\n');
    putint(umn(1u, 2u)); // 1
    put('\n');
    putint(umx(1u, 2u)); // 2
    put('\n');
    return 0;
}

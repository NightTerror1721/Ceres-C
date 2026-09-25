// 03 - every looping and branching form of the subset, and what each one lowers to.
//
// `if`, `while`, `do`/`while` and `for` all become the same two IR instructions in the end - a
// conditional branch and an unconditional jump between basic blocks (docs/03-IR-to-CASM.md). The
// difference between them is only WHERE the condition test lands, which `--emit-ir` shows better
// than any description:
//
//     ceresc examples/03_control_flow.c --emit-ir
//     ceresc examples/03_control_flow.c --run

void put(char c)
{
    volatile unsigned int* terminal = (volatile unsigned int*)0xFF000004;
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

char* classify(int n)
{
    if (n < 0)
        return "negative";
    else if (n == 0)
        return "zero";
    else
        return "positive";
}

int main(void)
{
    putstr("if/else:   ");
    putstr(classify(-4));
    put(' ');
    putstr(classify(0));
    put(' ');
    putstr(classify(7));
    put('\n');

    // while: the test happens before the first iteration, so a false condition runs the body zero
    // times.
    putstr("while:     ");
    int n = 1;
    while (n <= 64)
    {
        putint(n);
        put(' ');
        n = n * 2;
    }
    put('\n');

    // do/while: the body runs once before the test - the one loop form that cannot execute zero
    // times.
    putstr("do/while:  ");
    int countdown = 3;
    do
    {
        putint(countdown);
        put(' ');
        countdown--;
    } while (countdown > 0);
    put('\n');

    // for with break and continue: `continue` jumps to the increment (not to the test), which is
    // exactly how the IR builder wires the block up.
    putstr("for:       ");
    for (int i = 0; i < 20; i++)
    {
        if (i % 3 != 0)
            continue;
        if (i > 12)
            break;
        putint(i);
        put(' ');
    }
    put('\n');

    // Short-circuit: the right-hand side is never evaluated when the left already decides the
    // answer, so `steps` counts how many times `expensive` actually ran.
    putstr("shortcut:  ");
    int steps = 0;
    for (int i = 0; i < 4; i++)
    {
        if (i > 1 && ++steps > 0)
            put('Y');
        else
            put('.');
    }
    put(' ');
    putint(steps);
    put('\n');

    // Nested loops, with the inner one ending early.
    putstr("nested:    ");
    for (int row = 1; row <= 3; row++)
    {
        for (int col = 1; col <= 3; col++)
        {
            if (col > row)
                break;
            put('*');
        }
        put('|');
    }
    put('\n');

    return 0;
}

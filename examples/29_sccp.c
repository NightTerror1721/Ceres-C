// 29 - conditional constant propagation: a branch or a switch whose value is known at compile time.
//
// A `switch` over a dense case set compiles to a jump table, and a table is opaque to the ordinary
// constant folder - it sees a table dispatch, not a comparison. The conditional-constant pass
// propagates what it knows along the taken edges, so a switch on a constant collapses straight to
// its matching case (and a branch on a constant to its taken arm) without changing what the program
// computes. Expected: 103 100 42 7.
//
//     ceresc examples/29_sccp.c --run

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

int classify(int x)
{
    switch (x)
    {
        case 0: return 100;
        case 1: return 101;
        case 2: return 102;
        case 3: return 103;
        case 4: return 104;
        default: return -1;
    }
}

int main(void)
{
    int x = 3;
    int r;

    switch (x)
    {
        case 0: r = 200; break;
        case 1: r = 201; break;
        case 2: r = 202; break;
        case 3: r = 103; break;
        case 4: r = 204; break;
        default: r = -1; break;
    }
    putint(r); // 103 - the table collapses to the taken case
    put('\n');

    putint(classify(0)); // 100 - a call, so the switch runs at run time
    put('\n');

    if (2 + 2 == 4)
        putint(42); // 42 - the condition folds and the else arm is dropped
    else
        putint(0);
    put('\n');

    int y = 10;
    if (y > 5)
        putint(7); // 7
    else
        putint(9);
    put('\n');

    return 0;
}

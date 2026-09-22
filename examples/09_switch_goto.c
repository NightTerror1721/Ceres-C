// 09 - switch/case/default, fallthrough, and goto with labels.
//
// A `switch` is dispatched one of three ways, decided by IrBuilder (docs/13-Switch-Jump-Table-Plan.md):
// a dense case set becomes a jump table in `.rodata` (dayName, countDigits here), a sparse-but-large
// one a balanced tree of comparisons, and a small one the plain comparison chain. At -O0, or with
// -fno-jump-tables, every switch keeps the chain. `--emit-ir` shows which shape a given switch took.
//
// `case` labels wrap the one statement that follows them, exactly as in real C, so execution runs
// into the next case unless a `break` stops it.
//
// `goto` is resolved in two passes: a label may be used before the line that declares it, which is
// why sema registers every label in a function before resolving any jump to one.
//
//     ceresc examples/09_switch_goto.c --emit-ir     # the dispatch, block by block
//     ceresc examples/09_switch_goto.c --run

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

char* dayName(int day)
{
    switch (day)
    {
        case 1: return "monday";
        case 2: return "tuesday";
        case 3: return "wednesday";
        case 4: return "thursday";
        case 5: return "friday";
        case 6: return "saturday";
        case 7: return "sunday";
        default: return "not a day";
    }
}

// Deliberate fallthrough: 1, 2 and 3 all reach the same statement, and `default` is not last.
int weight(int grade)
{
    int score = 0;
    switch (grade)
    {
        case 1:
        case 2:
        case 3:
            score = score + 10;
            break;
        default:
            score = score - 1;
            break;
        case 9:
            score = score + 100;
            break;
    }
    return score;
}

// `break` inside a switch that sits inside a loop leaves the switch, not the loop.
int countDigits(char* text)
{
    int digits = 0;
    for (int i = 0; text[i] != 0; i++)
    {
        switch (text[i])
        {
            case 48:
            case 49:
            case 50:
            case 51:
            case 52:
            case 53:
            case 54:
            case 55:
            case 56:
            case 57:
                digits++;
                break;
            default:
                break;
        }
    }
    return digits;
}

int main(void)
{
    putstr("switch:    ");
    for (int day = 0; day <= 8; day++)
    {
        putstr(dayName(day));
        put(' ');
    }
    put('\n');

    putstr("weights:   ");
    for (int grade = 0; grade <= 9; grade++)
    {
        putint(weight(grade));
        put(' ');
    }
    put('\n');

    putstr("digits:    ");
    putint(countDigits("a1b22c333"));
    put('\n');

    // goto forwards: skipping the rest of a body is the one case where it reads better than the
    // flag variable it replaces.
    putstr("goto fwd:  ");
    for (int i = 0; i < 6; i++)
    {
        if (i == 3)
            goto done;
        putint(i);
        put(' ');
    }
done:
    putstr("stopped at 3\n");

    // goto backwards: a loop written by hand, out of a conditional jump and a label.
    putstr("goto back: ");
    int n = 1;
again:
    putint(n);
    put(' ');
    n = n * 3;
    if (n < 100)
        goto again;
    put('\n');

    // A label used before it is declared, which a single-pass resolver could not accept.
    putstr("forward:   ");
    goto end;
    putstr("never printed");
end:
    putstr("jumped over the line above\n");

    return 0;
}

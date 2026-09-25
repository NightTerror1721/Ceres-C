// 23 - GNU case ranges: `case low ... high:`.
//
// Every value in the range shares one body, which the compiler expands into individual dispatch
// entries before choosing a jump table, a tree or a chain (docs/13). Printed: 1, 2, 3, 4.
//
//     ceresc examples/23_case_range.c --run

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

int classify(int n)
{
    switch (n)
    {
        case 0 ... 9:    return 1; // one digit
        case 10 ... 99:  return 2; // two digits
        case -9 ... -1:  return 3; // a small negative
        default:         return 4;
    }
}

int main(void)
{
    putint(classify(5));    put('\n'); // 1
    putint(classify(42));   put('\n'); // 2
    putint(classify(-3));   put('\n'); // 3
    putint(classify(1000)); put('\n'); // 4
    return 0;
}

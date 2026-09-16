// 05 - arrays, initializer lists and two-dimensional indexing.
//
// The ISA has no "base + index * scale" addressing mode, so `a[i]` costs one extra instruction:
// the index is scaled by the element size first, then the indexed load/store form does the rest
// (docs/03-IR-to-CASM.md). A 2D array is a single flat block - `m[row][col]` is two scalings, one
// by the row stride and one by the element size, not a pointer chase through a row table.
//
//     ceresc examples/05_arrays.c --run

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

void putrow(int* values, int count)
{
    for (int i = 0; i < count; i++)
    {
        putint(values[i]);
        put(' ');
    }
    put('\n');
}

int main(void)
{
    // A full initializer list.
    int primes[6] = { 2, 3, 5, 7, 11, 13 };
    putstr("primes:    ");
    putrow(primes, 6);

    // A partial one: C fills what the list did not reach with zeros, and Ceres-C emits those
    // stores itself since there is no memset to call.
    int sparse[6] = { 9, 8 };
    putstr("sparse:    ");
    putrow(sparse, 6);

    // Uninitialized, then filled by a loop.
    int squares[6];
    for (int i = 0; i < 6; i++)
        squares[i] = i * i;
    putstr("squares:   ");
    putrow(squares, 6);

    // An array decays to a pointer to its first element when passed to a function, which is why
    // `putrow` takes an `int*` and needs the length told to it separately.
    putstr("sum:       ");
    int total = 0;
    for (int i = 0; i < 6; i++)
        total = total + primes[i];
    putint(total);
    put('\n');

    // Element sizes differ, so the scaling does too: one byte per char, four per int.
    char letters[5] = { 'C', 'e', 'r', 'e', 's' };
    putstr("letters:   ");
    for (int i = 0; i < 5; i++)
        put(letters[i]);
    put('\n');

    // Two dimensions. The row stride is 4 ints = 16 bytes.
    int grid[3][4] = { { 1, 2, 3, 4 }, { 5, 6, 7, 8 }, { 9, 10, 11, 12 } };
    putstr("grid:\n");
    for (int row = 0; row < 3; row++)
    {
        putstr("  ");
        for (int col = 0; col < 4; col++)
        {
            putint(grid[row][col]);
            put(' ');
        }
        put('\n');
    }

    // The diagonal, to show the two indices really are independent.
    putstr("diagonal:  ");
    for (int i = 0; i < 3; i++)
    {
        putint(grid[i][i]);
        put(' ');
    }
    put('\n');

    putstr("sizeof:    ");
    putint((int)sizeof(primes));
    putstr(" bytes for 6 ints, ");
    putint((int)sizeof(grid));
    putstr(" for a 3x4 grid\n");

    return 0;
}

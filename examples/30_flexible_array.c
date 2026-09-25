// 30 - flexible array members: `struct S { int n; int a[]; };`.
//
// The last member of a struct may be an unsized array. It contributes no bytes to the struct's own
// size - `sizeof(struct S)` is just the fixed part - and its own address is the first byte after
// that, so a program that allocates a larger block and casts it to the struct can reach a tail of
// as many elements as it reserved. This is how a length-prefixed buffer is built in C. Expected:
// 4 4 14 5.
//
//     ceresc examples/30_flexible_array.c --run

struct Ints
{
    int count;
    int values[];
};

struct Padded
{
    char tag;
    int values[]; // aligned to 4, so the fixed part is four bytes, not one
};

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

int main(void)
{
    putint(sizeof(struct Ints)); // 4 - the member is not counted
    put('\n');
    putint(sizeof(struct Padded)); // 4 - `tag` is padded up to the member's alignment
    put('\n');

    // Room for the fixed part plus four elements. The storage is an `int` array so it is
    // word-aligned: this machine faults on a four-byte access that is not.
    int storage[5];
    struct Ints* box = (struct Ints*)storage;
    box->count = 4;
    for (int i = 0; i < box->count; i = i + 1)
        box->values[i] = i * i;

    int sum = 0;
    for (int i = 0; i < box->count; i = i + 1)
        sum = sum + box->values[i];
    putint(sum); // 0 + 1 + 4 + 9 = 14
    put('\n');

    // The member decays to a pointer, so it can be passed around and indexed like any other.
    int* tail = box->values;
    putint(tail[1] + tail[2]); // 1 + 4 = 5
    put('\n');

    return 0;
}

// 31 - walking an array by advancing a pointer instead of recomputing `base + i*C`.
//
// The three loops below are the shapes induction-variable strength reduction (docs/14 O3) fires
// on: an int array (stride 4, a power of two), a struct array (stride 12, a multiply that is NOT a
// power of two), and a reverse walk from a non-zero index (step -1). In each, the address the loop
// loads through is an induction variable of its own, so the pass replaces the per-iteration
// multiply and address addition with a pointer that starts before the loop and is bumped in the
// latch.
//
// The printed values are computed by hand, so a pass that changed the meaning of any loop rather
// than its cost would show up as a wrong number:
//
//   words[0..5]     = 1 2 3 4 5 6
//   triples[i].a    = 10 20 30 40
//
//   sum_words(words, 6, 2)  = words[2]+words[3]+words[4]+words[5] = 3+4+5+6 = 18
//   sum_triple_a(triples,4)              = 10+20+30+40        = 100
//   sum_back(words, 5)      = words[5]+...+words[1]           = 6+5+4+3+2 = 20
//
//     ceresc examples/31_induction_vars.c --run

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

struct Triple
{
    int a;
    int b;
    int c;
};

int sum_words(int* arr, int n, int start)
{
    int total = 0;
    for (int i = start; i < n; i = i + 1)
        total = total + arr[i];
    return total;
}

int sum_triple_a(struct Triple* arr, int n)
{
    int total = 0;
    for (int i = 0; i < n; i = i + 1)
        total = total + arr[i].a;
    return total;
}

int sum_back(int* arr, int from)
{
    int total = 0;
    for (int i = from; i > 0; i = i - 1)
        total = total + arr[i];
    return total;
}

int main(void)
{
    int words[6];
    words[0] = 1;
    words[1] = 2;
    words[2] = 3;
    words[3] = 4;
    words[4] = 5;
    words[5] = 6;

    struct Triple triples[4];
    for (int i = 0; i < 4; i = i + 1)
    {
        triples[i].a = (i + 1) * 10;
        triples[i].b = 0;
        triples[i].c = 0;
    }

    putint(sum_words(words, 6, 2)); // 18
    put('\n');
    putint(sum_triple_a(triples, 4)); // 100
    put('\n');
    putint(sum_back(words, 5)); // 20
    put('\n');

    return 0;
}

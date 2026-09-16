// 19 - function pointers: choosing what to call while the program is running.
//
// Every call in the previous eighteen examples names its callee, and the linker resolves that name
// to an address once, before the program starts. A function pointer holds the address instead, so
// the choice can be made by a value:
//
//     int (*op)(int, int) = add;      // op holds add's address
//     op(3, 4);                       // call rN, not call add
//
// The declaration reads outside-in: `op` is a pointer (`*op`), to a function (`(int, int)`),
// returning `int`. The parentheses around `*op` are not decoration - without them,
// `int *op(int, int)` declares a function returning `int*`, which is a different thing entirely.
// That one pair of parentheses is the whole difference, and it is why C's declarator syntax looks
// the way it does.
//
// `add` on its own, with no call after it, is the function's address: a function decays to a
// pointer to itself exactly as an array decays to a pointer to its first element, so `add` and
// `&add` mean the same thing.
//
// A `typedef` can name either half. `Binary` below is the function TYPE - the signature itself -
// so `Binary*` is a pointer to one, and the two spellings are the same type rather than two that
// happen to agree.
//
//     ceresc examples/19_function_pointers.c --run

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
        value = 0 - value;
    }
    if (value >= 10)
        putint(value / 10);
    put((char)('0' + value % 10));
}

// ---- the operations ----------------------------------------------------------------------------

int add(int a, int b) { return a + b; }
int sub(int a, int b) { return a - b; }
int mul(int a, int b) { return a * b; }

// The signature itself has a name, so `Binary*` below is a pointer to one of these three.
typedef int Binary(int a, int b);

// ---- taking one as an argument -------------------------------------------------------------------

// The parameter is a pointer to a function, however it is spelled: `Binary* op` and
// `int (*op)(int, int)` declare the same thing, and so would `int op(int, int)` - a function
// parameter decays for the same reason an array one does.
int apply(Binary* op, int a, int b)
{
    return op(a, b);
}

// ---- and returning one ---------------------------------------------------------------------------

// `chooseOp` is a function taking a char and returning a pointer to a function of two ints. Read
// from the name outward: `chooseOp(char)` is a function, `*` says it returns a pointer, and
// `(int, int)` plus the leading `int` describe what that pointer points at.
int (*chooseOp(char symbol))(int, int)
{
    if (symbol == '+') return add;
    if (symbol == '-') return sub;
    return mul;
}

int main(void)
{
    // An array of function pointers - a dispatch table, which is the reason this feature exists.
    // `table` is an array (`[3]`) of pointers (`*`) to functions of two ints.
    int (*table[3])(int, int);
    table[0] = add;
    table[1] = sub;
    table[2] = mul;

    char symbols[3];
    symbols[0] = '+';
    symbols[1] = '-';
    symbols[2] = '*';

    putstr("table:  ");
    for (int i = 0; i < 3; i++)
    {
        putstr("12 ");
        put(symbols[i]);
        putstr(" 5 = ");
        putint(table[i](12, 5));
        putstr(i < 2 ? ", " : "\n");
    }

    putstr("apply:  ");
    putint(apply(add, 20, 22));
    putstr(" and ");
    putint(apply(mul, 6, 7));
    put('\n');

    putstr("chosen: ");
    for (int i = 0; i < 3; i++)
    {
        putint(chooseOp(symbols[i])(9, 4));
        putstr(i < 2 ? ", " : "\n");
    }

    // A pointer is a value like any other: it compares, and it can be null.
    int (*maybe)(int, int) = 0;
    putstr("null:   ");
    putstr(maybe == 0 ? "yes" : "no");
    put('\n');

    maybe = table[0];
    putstr("same:   ");
    putstr(maybe == add ? "yes" : "no");
    put('\n');

    return 0;
}

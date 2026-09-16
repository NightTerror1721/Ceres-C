// 07 - structs: layout, the two member operators, and passing them around.
//
// The exit criterion of the phased plan names this file: `ceresc examples/07_structs.c --run`
// should work on a fresh checkout without reading a line of the compiler's source.
//
// A struct's layout follows the same rule CeresASM's own `struct` directive uses
// (docs/23-Structs.md): every field is aligned to its own size, and the total is rounded up to
// the widest field. That is why `Padded` below is twelve bytes and not six - the generated .casm
// contains a `struct` directive describing each frame, so the layout is readable there too.
//
// `.` and `->` are genuinely different operators here, not one spelled two ways: `p.x` needs a
// struct, `p->x` needs a pointer to one.
//
//     ceresc examples/07_structs.c --run

struct Point
{
    int x;
    int y;
};

// char, then int: the int cannot start at offset 1, so three bytes of padding go in between.
struct Padded
{
    char tag;
    int value;
    char flag;
};

struct Line
{
    struct Point from;
    struct Point to;
};

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

void putPoint(struct Point* p)
{
    put('(');
    putint(p->x);
    put(',');
    putint(p->y);
    put(')');
}

// Filling through a pointer: the caller's object is written in place.
void setPoint(struct Point* p, int x, int y)
{
    p->x = x;
    p->y = y;
}

// By value. The callee gets a copy, so writing to it cannot be seen by the caller.
int manhattan(struct Point p)
{
    if (p.x < 0)
        p.x = -p.x;
    if (p.y < 0)
        p.y = -p.y;
    return p.x + p.y;
}

// Returned by value. Anything wider than a word comes back through a hidden destination pointer
// the caller supplies; one word or less comes back in ret0.
struct Point midpoint(struct Line line)
{
    struct Point result;
    result.x = (line.from.x + line.to.x) / 2;
    result.y = (line.from.y + line.to.y) / 2;
    return result;
}

int main(void)
{
    struct Point origin;
    setPoint(&origin, 0, 0);
    struct Point corner;
    setPoint(&corner, 6, 8);

    putstr("points:      ");
    putPoint(&origin);
    put(' ');
    putPoint(&corner);
    put('\n');

    putstr("by value:    manhattan(-3,4) = ");
    struct Point negative;
    setPoint(&negative, -3, 4);
    putint(manhattan(negative));
    putstr(", and the original is still ");
    putPoint(&negative);
    put('\n');

    // Whole-struct assignment copies every field.
    putstr("assignment:  ");
    struct Point copy;
    copy = corner;
    copy.x = 1;
    putPoint(&copy);
    putstr(" from ");
    putPoint(&corner);
    put('\n');

    // Nested structs are reached through two constant offsets, not a pointer hop.
    struct Line diagonal = { { 0, 0 }, { 6, 8 } };
    putstr("nested:      from ");
    putPoint(&diagonal.from);
    putstr(" to ");
    putPoint(&diagonal.to);
    put('\n');

    putstr("returned:    midpoint = ");
    struct Point middle = midpoint(diagonal);
    putPoint(&middle);
    put('\n');

    // An array of structs strides by the whole struct, padding included.
    struct Point path[4];
    for (int i = 0; i < 4; i++)
        setPoint(&path[i], i, i * i);
    putstr("array:       ");
    for (int i = 0; i < 4; i++)
    {
        putPoint(&path[i]);
        put(' ');
    }
    put('\n');

    // `.` on the object, `->` through a pointer to it - the same field either way.
    struct Point* cursor = &path[2];
    putstr("dot/arrow:   ");
    putint(path[2].y);
    putstr(" == ");
    putint(cursor->y);
    put('\n');

    putstr("layout:      sizeof(Point) = ");
    putint((int)sizeof(struct Point));
    putstr(", sizeof(Padded) = ");
    putint((int)sizeof(struct Padded));
    putstr(" (6 bytes of fields, padded to 12)\n");

    return 0;
}

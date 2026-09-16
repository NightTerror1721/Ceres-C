// 12 - globals, enums and where each one ends up in the generated image.
//
// A global with an initializer becomes a `@data` entry holding its bytes; one without becomes a
// `@bss` reservation the loader zeroes. Neither lives in a frame, so a global keeps its value
// across calls - that is the whole difference from a local, and the reason the optimizer must
// reload one after any call that could have changed it.
//
// An enum is a set of named int constants, not a distinct type at runtime: the values are folded
// into the code and nothing about them survives into the .casm.
//
//     ceresc examples/12_globals_and_enums.c -o 12.casm    # look for @data and @bss
//     ceresc examples/12_globals_and_enums.c --run

enum Level
{
    Silent,          // 0 by default
    Normal,          // 1 - each enumerator is the previous one plus one
    Loud = 10,       // an explicit value restarts the counting
    Deafening        // 11
};

enum Mask
{
    None = 0,
    Read = 1,
    Write = 2,
    Execute = 4
};

struct Counter
{
    int calls;
    int total;
};

// Initialized: goes to @data.
int callCount = 0;
int scale = 3;
int table[5] = { 1, 1, 2, 3, 5 };
char banner[8] = "ceres";

// Uninitialized: goes to @bss, zeroed by the loader.
struct Counter statistics;
int scratch[4];

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

// Touches two globals and is called several times - the state is in the globals, not in the
// caller.
int accumulate(int amount)
{
    callCount++;
    statistics.calls++;
    statistics.total = statistics.total + amount * scale;
    return statistics.total;
}

char* levelName(enum Level level)
{
    if (level == Silent)
        return "silent";
    if (level == Normal)
        return "normal";
    if (level == Loud)
        return "loud";
    return "deafening";
}

int main(void)
{
    putstr("enum values:  ");
    putint(Silent);
    put(' ');
    putint(Normal);
    put(' ');
    putint(Loud);
    put(' ');
    putint(Deafening);
    put('\n');

    putstr("enum names:   ");
    putstr(levelName(Silent));
    put(' ');
    putstr(levelName(Deafening));
    put('\n');

    // Enumerators are plain ints, so they combine with the bitwise operators like any other.
    int permissions = Read | Write;
    putstr("flags:        ");
    putint(permissions);
    putstr(" - readable: ");
    putint((permissions & Read) != 0);
    putstr(", executable: ");
    putint((permissions & Execute) != 0);
    put('\n');

    putstr("@data:        ");
    putstr(banner);
    putstr(", table = ");
    for (int i = 0; i < 5; i++)
    {
        putint(table[i]);
        put(' ');
    }
    put('\n');

    putstr("@bss:         ");
    putint(statistics.calls);
    put(' ');
    putint(statistics.total);
    putstr(" (zeroed by the loader, never written by the program)");
    put('\n');

    putstr("accumulate:   ");
    putint(accumulate(1));
    put(' ');
    putint(accumulate(2));
    put(' ');
    putint(accumulate(3));
    put('\n');

    putstr("state kept:   ");
    putint(callCount);
    putstr(" calls, total ");
    putint(statistics.total);
    put('\n');

    // A global array is just as writable as a local one.
    for (int i = 0; i < 4; i++)
        scratch[i] = table[i] * scale;
    putstr("written bss:  ");
    for (int i = 0; i < 4; i++)
    {
        putint(scratch[i]);
        put(' ');
    }
    put('\n');

    return 0;
}

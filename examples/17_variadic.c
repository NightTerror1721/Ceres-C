// 17 - variadic functions: `...`, __builtin_va_list, and the printf everyone writes once.
//
// A parameter list ending in `...` accepts arguments the declaration does not describe. The fixed
// parameters are passed the ordinary way; everything past them - the "tail" - always goes on the
// stack, which is what lets the callee find it at all. A __builtin_va_list is a cursor into that
// tail, and __builtin_va_arg reads one argument and steps forward.
//
// The rule this file exists to make concrete: the callee cannot know the types of its tail, so
// SOMETHING has to tell it. Here, as in real C, it is the format string - and getting that wrong is
// not a compile error, it is a wrong answer. `%d` against a float argument reads the float's bits as
// an integer, and nothing will stop you.
//
// `__builtin_va_list`, `__builtin_va_start`, `__builtin_va_arg`, `__builtin_va_end` and
// `__builtin_va_copy` are builtin: there is no <stdarg.h> to include, because there is no system
// include directory to find one in. The full contract is in docs/09-Variadic-Convention.md.
//
//     ceresc examples/17_variadic.c --run

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

// The simplest useful variadic function: one fixed parameter saying how many follow.
int sum(int count, ...)
{
    __builtin_va_list arguments;
    int total = 0;

    __builtin_va_start(arguments, count);
    for (int i = 0; i < count; i++)
        total = total + __builtin_va_arg(arguments, int);
    __builtin_va_end(arguments);
    return total;
}

// __builtin_va_copy exists because a __builtin_va_list can only be walked forwards. Copy it first
// and the same tail can be read twice - here, once to find the largest and once to print them all.
int largest(int count, ...)
{
    __builtin_va_list arguments;
    __builtin_va_list replay;
    int best;

    __builtin_va_start(arguments, count);
    __builtin_va_copy(replay, arguments);

    best = __builtin_va_arg(arguments, int);
    for (int i = 1; i < count; i++)
    {
        int value = __builtin_va_arg(arguments, int);
        if (value > best)
            best = value;
    }
    __builtin_va_end(arguments);

    putstr("of ");
    for (int i = 0; i < count; i++)
    {
        putint(__builtin_va_arg(replay, int));
        put(' ');
    }
    __builtin_va_end(replay);
    return best;
}

// Six fixed parameters, so only four of them fit in argument registers: `e` and `f` already
// arrived on the stack. The tail therefore cannot start at the first incoming stack word - it
// starts after the ones the fixed parameters took. Both sides work that out from the fixed
// parameter list alone, which is why it stays correct without anything being passed at runtime.
int after_spilled_fixed(int a, int b, int c, int d, int e, int f, ...)
{
    __builtin_va_list arguments;
    int total = a + b + c + d + e + f;

    __builtin_va_start(arguments, f);
    for (int i = 0; i < 3; i++)
        total = total + __builtin_va_arg(arguments, int);
    __builtin_va_end(arguments);
    return total;
}

// A __builtin_va_list is an ordinary pointer, so it can be handed on - which is how the real
// <stdio.h> gets both printf and vprintf out of one implementation. This is the vprintf half.
void vformat(char* format, __builtin_va_list arguments)
{
    for (int i = 0; format[i] != 0; i++)
    {
        if (format[i] != '%')
        {
            put(format[i]);
            continue;
        }

        i++;
        if (format[i] == 'd')
            putint(__builtin_va_arg(arguments, int));
        else if (format[i] == 'c')
            put((char)__builtin_va_arg(arguments, int));
        else if (format[i] == 's')
            putstr(__builtin_va_arg(arguments, char*));
        else if (format[i] == 'f')
            putint((int)__builtin_va_arg(arguments, float)); // truncated: no f64 here, no %.2f
        else
            put(format[i]);                        // "%%" prints one '%'
    }
}

// ...and this is the printf half, which is now four lines.
void format(char* format_, ...)
{
    __builtin_va_list arguments;
    __builtin_va_start(arguments, format_);
    vformat(format_, arguments);
    __builtin_va_end(arguments);
}

int main(void)
{
    putstr("sum:        ");
    putint(sum(3, 10, 20, 30));
    putstr(" and ");
    putint(sum(5, 1, 2, 3, 4, 5));
    putstr(" and ");
    putint(sum(0));               // a tail may be empty
    put('\n');

    putstr("largest:    ");
    int best = largest(4, 12, 99, 7, 40);
    putstr("-> ");
    putint(best);
    put('\n');

    putstr("format:     ");
    format("%d items, %c grade, %s, %d%%\n", 42, 'A', "all present", 100);

    // Mixed banks: an int and a float in the same tail. Both travel as one stack word each, so the
    // float arrives as the f32 it already is - C would have promoted it to double, and there is no
    // double here to promote it to.
    putstr("mixed:      ");
    format("int %d then float %f then int %d\n", 7, 2.75, 9);

    putstr("spilled:    ");
    putint(after_spilled_fixed(1, 2, 3, 4, 5, 6, 10, 20, 30));
    putstr(" (21 fixed + 60 variadic)\n");

    return 0;
}

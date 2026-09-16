// 08 - string literals, char arrays, and writing the handful of routines <string.h> would give
// you in a language that had one.
//
// A string literal is bytes in `.rodata` and a `char*` to them: read-only, shared, and the same
// pointer every time it appears. A `char array[] = "..."` declaration is different - the array
// owns its own copy of the bytes on the stack, so it can be written to.
//
//     ceresc examples/08_strings.c --run

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

int stringLength(char* text)
{
    int length = 0;
    while (text[length] != 0)
        length++;
    return length;
}

// Returns a negative number, zero or a positive one, the same three-way answer strcmp gives.
int compare(char* left, char* right)
{
    int i = 0;
    while (left[i] != 0 && left[i] == right[i])
        i++;
    return left[i] - right[i];
}

void copy(char* destination, char* source)
{
    int i = 0;
    while (source[i] != 0)
    {
        destination[i] = source[i];
        i++;
    }
    destination[i] = 0;
}

void reverseInPlace(char* text)
{
    int last = stringLength(text) - 1;
    for (int i = 0; i < last; i++)
    {
        char temp = text[i];
        text[i] = text[last];
        text[last] = temp;
        last--;
    }
}

void toUpper(char* text)
{
    for (int i = 0; text[i] != 0; i++)
    {
        if (text[i] >= 'a' && text[i] <= 'z')
            text[i] = text[i] - 32;
    }
}

int countVowels(char* text)
{
    int count = 0;
    for (int i = 0; text[i] != 0; i++)
    {
        char c = text[i];
        if (c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u')
            count++;
    }
    return count;
}

int main(void)
{
    char* literal = "ceres compiler";

    putstr("literal:   ");
    putstr(literal);
    put('\n');

    putstr("length:    ");
    putint(stringLength(literal));
    put('\n');

    putstr("vowels:    ");
    putint(countVowels(literal));
    put('\n');

    // Escapes the lexer understands: \n \t \0 \ \' \"
    putstr("escapes:   tab[\t] quote[\"] backslash[\\]\n");

    // A char array owns its bytes, so these routines may write to it. The literal above may not.
    char buffer[32];
    copy(buffer, literal);
    toUpper(buffer);
    putstr("upper:     ");
    putstr(buffer);
    put('\n');

    reverseInPlace(buffer);
    putstr("reversed:  ");
    putstr(buffer);
    put('\n');

    // Initialized from a literal: the array holds a copy, and the terminating zero is really
    // there, which is why `sizeof` is one more than the visible length.
    char greeting[8] = "hello";
    putstr("copy:      ");
    putstr(greeting);
    putstr(" (length ");
    putint(stringLength(greeting));
    putstr(", array of ");
    putint((int)sizeof(greeting));
    putstr(")\n");

    greeting[0] = 'j';
    putstr("mutated:   ");
    putstr(greeting);
    put('\n');

    putstr("compare:   ");
    putint(compare("abc", "abc"));
    put(' ');
    putint(compare("abc", "abd"));
    put(' ');
    putint(compare("abd", "abc"));
    put('\n');

    return 0;
}

// 11 - single-precision floating point.
//
// The VM has a second bank of sixteen 32-bit IEEE-754 registers, f0-f15, and Ceres-C uses it the
// same way it uses the integer bank: f4-f7 as scratch, everything else in memory between
// statements. There is no `double` - f64 has no hardware support here at all.
//
// Conversion is explicit machinery, not a reinterpretation: `(int)x` emits a convert instruction
// that truncates toward zero, and an int used where a float is expected converts the other way.
//
//     ceresc examples/11_floats.c -o 11.casm    # look for the f-register instructions
//     ceresc examples/11_floats.c --run

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

// There is no printf, so a float is printed by splitting it into a whole part and a scaled
// fractional one. `decimals` digits after the point, truncated rather than rounded.
void putfloat(float value, int decimals)
{
    if (value < 0.0)
    {
        put('-');
        value = -value;
    }
    int whole = (int)value;
    putint(whole);
    put('.');
    float fraction = value - (float)whole;
    for (int i = 0; i < decimals; i++)
    {
        fraction = fraction * 10.0;
        int digit = (int)fraction;
        put('0' + digit);
        fraction = fraction - (float)digit;
    }
}

float average(float* values, int count)
{
    float total = 0.0;
    for (int i = 0; i < count; i++)
        total = total + values[i];
    return total / (float)count;
}

// Newton's method: no sqrt instruction is reachable from the C subset, so this is how you get one.
float squareRoot(float value)
{
    if (value <= 0.0)
        return 0.0;
    float guess = value;
    for (int i = 0; i < 20; i++)
        guess = (guess + value / guess) / 2.0;
    return guess;
}

int main(void)
{
    float a = 7.5;
    float b = 2.0;

    putstr("arithmetic: ");
    putfloat(a + b, 3);
    put(' ');
    putfloat(a - b, 3);
    put(' ');
    putfloat(a * b, 3);
    put(' ');
    putfloat(a / b, 3);
    put('\n');

    putstr("negate:     ");
    putfloat(-a, 3);
    put('\n');

    // Comparison uses the float compare, not the integer one.
    putstr("compare:    ");
    if (a > b)
        putstr("7.5 > 2.0");
    putstr("; ");
    if (b < 1.0 == false)
        putstr("2.0 is not below 1.0");
    put('\n');

    // int -> float widens exactly; float -> int truncates toward zero, in both directions.
    putstr("convert:    ");
    int n = 9;
    float widened = (float)n / 2.0;
    putfloat(widened, 3);
    putstr(" -> ");
    putint((int)widened);
    putstr(", and -4.75 -> ");
    putint((int)-4.75);
    put('\n');

    putstr("exponent:   ");
    putfloat(1.5e3, 1);
    put(' ');
    putfloat(2.5e-2, 4);
    put('\n');

    float samples[5] = { 1.5, 2.25, 3.0, 4.75, 5.5 };
    putstr("average:    ");
    putfloat(average(samples, 5), 4);
    put('\n');

    putstr("sqrt(2):    ");
    putfloat(squareRoot(2.0), 6);
    put('\n');

    // Rounding a positive value: add a half, then truncate.
    putstr("rounded:    ");
    for (int i = 0; i < 5; i++)
    {
        float value = (float)i * 0.75;
        putint((int)(value + 0.5));
        put(' ');
    }
    put('\n');

    return 0;
}

// 26 - tail calls: `return f(args)` jumps to f instead of call+ret.
//
// A call whose result is returned unchanged needs no return address of its own: the caller's own
// return address is already where the callee's `ret` will pop it. The compiler moves the arguments
// into place, restores the frame and the callee-saved registers, and jumps - so a tail-recursive
// loop runs in constant stack space, and mutual recursion does not grow the stack either. Expected:
// 55 then 1 then 1.
//
//     ceresc examples/26_tail_calls.c --run

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

// The result of the recursive call IS the result, so the frame does not have to survive it.
int sumTo(int n, int acc)
{
    if (n == 0)
        return acc;
    return sumTo(n - 1, acc + n);
}

int isOdd(int n);

int isEven(int n)
{
    if (n == 0)
        return 1;
    return isOdd(n - 1);
}

int isOdd(int n)
{
    if (n == 0)
        return 0;
    return isEven(n - 1);
}

int main(void)
{
    putint(sumTo(10, 0)); // 10+9+...+1 = 55
    put('\n');
    putint(isEven(10)); // 1
    put('\n');
    putint(isOdd(7)); // 1
    put('\n');
    return 0;
}

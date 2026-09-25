// 16 - typedef, and a small data structure built out of everything the subset has.
//
// A `typedef` is an alias the parser records, not a new type: `Stack` and `struct Stack` are the
// same bits and the same layout. From the declaration onwards the parser treats the name exactly
// like a type keyword, which is why `Stack` can be used before `struct` here and after it there.
//
// This file is the one that looks like ordinary C rather than a feature demonstration: a fixed
// capacity stack, a queue over a ring buffer, and a use for each - the subset is small, but it is
// large enough to write with.
//
//     ceresc examples/16_typedef_stack.c --run

typedef int Value;
typedef unsigned int Size;

struct Stack
{
    Value items[16];
    int count;
};
typedef struct Stack Stack;

struct Queue
{
    Value items[8];
    int head;
    int tail;
    int count;
};
typedef struct Queue Queue;

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

void putint(Value value)
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

void stackInit(Stack* stack)
{
    stack->count = 0;
}

bool stackPush(Stack* stack, Value value)
{
    if (stack->count >= 16)
        return false;
    stack->items[stack->count] = value;
    stack->count++;
    return true;
}

// Returns false when empty; the value comes back through the out-parameter, which is how a
// function returns two things in C.
bool stackPop(Stack* stack, Value* out)
{
    if (stack->count == 0)
        return false;
    stack->count--;
    *out = stack->items[stack->count];
    return true;
}

void queueInit(Queue* queue)
{
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
}

bool queuePush(Queue* queue, Value value)
{
    if (queue->count >= 8)
        return false;
    queue->items[queue->tail] = value;
    queue->tail = (queue->tail + 1) % 8;
    queue->count++;
    return true;
}

bool queuePop(Queue* queue, Value* out)
{
    if (queue->count == 0)
        return false;
    *out = queue->items[queue->head];
    queue->head = (queue->head + 1) % 8;
    queue->count--;
    return true;
}

// The classic use for a stack: reverse without a second buffer.
void reverseThroughStack(Value* values, int count)
{
    Stack stack;
    stackInit(&stack);
    for (int i = 0; i < count; i++)
        stackPush(&stack, values[i]);
    for (int i = 0; i < count; i++)
    {
        Value popped = 0;
        stackPop(&stack, &popped);
        values[i] = popped;
    }
}

// Balanced-bracket checking, the other classic one.
bool bracketsBalanced(char* text)
{
    Stack stack;
    stackInit(&stack);
    for (int i = 0; text[i] != 0; i++)
    {
        char c = text[i];
        if (c == '(' || c == '[')
        {
            stackPush(&stack, c);
        }
        else if (c == ')' || c == ']')
        {
            Value opening = 0;
            if (stackPop(&stack, &opening) == false)
                return false;
            if (c == ')' && opening != '(')
                return false;
            if (c == ']' && opening != '[')
                return false;
        }
    }
    return stack.count == 0;
}

int main(void)
{
    putstr("typedef:    Size is ");
    putint((Value)sizeof(Size));
    putstr(" bytes, Stack is ");
    putint((Value)sizeof(Stack));
    putstr(", and `Stack` == `struct Stack`\n");

    Stack stack;
    stackInit(&stack);
    putstr("stack:      pushing 1..5, popping ");
    for (Value v = 1; v <= 5; v++)
        stackPush(&stack, v);
    Value popped = 0;
    while (stackPop(&stack, &popped))
    {
        putint(popped);
        put(' ');
    }
    putstr("| empty pop returns ");
    putint(stackPop(&stack, &popped));
    put('\n');

    Queue queue;
    queueInit(&queue);
    putstr("queue:      pushing 1..5, popping ");
    for (Value v = 1; v <= 5; v++)
        queuePush(&queue, v);
    Value taken = 0;
    while (queuePop(&queue, &taken))
    {
        putint(taken);
        put(' ');
    }
    put('\n');

    // The ring really wraps: eight in, four out, four more in, and the order stays right.
    putstr("wrap:       ");
    queueInit(&queue);
    for (Value v = 1; v <= 8; v++)
        queuePush(&queue, v);
    for (int i = 0; i < 4; i++)
        queuePop(&queue, &taken);
    for (Value v = 9; v <= 12; v++)
        queuePush(&queue, v);
    while (queuePop(&queue, &taken))
    {
        putint(taken);
        put(' ');
    }
    put('\n');

    Value numbers[6] = { 1, 2, 3, 4, 5, 6 };
    reverseThroughStack(numbers, 6);
    putstr("reversed:   ");
    for (int i = 0; i < 6; i++)
    {
        putint(numbers[i]);
        put(' ');
    }
    put('\n');

    putstr("brackets:   ");
    putint(bracketsBalanced("([][()])"));
    put(' ');
    putint(bracketsBalanced("([)]"));
    put(' ');
    putint(bracketsBalanced("(("));
    put(' ');
    putint(bracketsBalanced(""));
    put('\n');

    return 0;
}

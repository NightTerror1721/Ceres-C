// 18 - interrupts: a function the machine calls, at a moment the program did not choose.
//
// Everything else in this directory runs because something called it. A handler does not. The
// machine stops whatever was executing, pushes the flags and the program counter, and jumps - and
// when the handler is done, the interrupted code resumes as though nothing happened.
//
// Two declarations, kept apart on purpose, exactly as CeresASM keeps them apart:
//
//     __interrupt void f(void) { ... }        // this is a handler: how it is entered and left
//     __interrupt_vector(NUMBER, f);          // this number is answered by that handler
//
// Splitting them is what lets a library publish a handler while the program that uses it picks the
// vector - and what lets a handler written here be bound from hand-written assembly, or the other
// way round (docs/07-CASM-Interop.md).
//
// A handler takes no arguments and returns nothing, because nobody is there to pass or read one.
// You cannot call it either: the generated code ends in `iret`, which pops the flags and PC the
// machine pushed, and a `call` would leave it popping a return address instead.
//
// `fired` is `volatile` and that is the whole point of the flag. Nothing in `main` writes it, so an
// optimizer reading `main` alone is entitled to decide the loop condition cannot change and spin
// forever. `volatile` says: this object changes behind your back, read it every time.
//
// The three instructions this needs have no C spelling, so they are builtins:
//
//     __builtin_sti()    unmask user interrupts - without it the timer's is dropped, not queued
//     __builtin_halt()   stop fetching until an interrupt arrives
//     __builtin_cli()    mask them again
//
// See docs/10-Interrupts.md for the vector table, the numbers and what a handler must preserve.
//
//     ceresc examples/18_interrupts.c --run

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

// The timer is the machine's one source of asynchronous interrupts, and it counts INSTRUCTIONS
// rather than milliseconds - so a program that arms it behaves identically on every run, and at
// every optimization level.
enum Timer
{
    Vector      = 16,         // UserInterrupt0, which is the timer's and nothing else's
    CommandPort = 0xFF010008, // writing a count arms it; writing 0 disarms it
    Delay       = 400         // instructions from now
};

// Written by the handler, read by main, and by nothing else in either direction. Without
// `volatile` the loop below is entitled to read it once and spin on the answer forever.
volatile int fired;

__interrupt void timer_isr(void)
{
    fired = 1;
}

__interrupt_vector(Vector, timer_isr);

int main(void)
{
    int* command = (int*)CommandPort;

    putstr("arming the timer\n");
    *command = Delay;

    // Unmask. A user interrupt that arrives while they are masked is dropped, not queued, so this
    // has to happen before the timer expires rather than after.
    __builtin_sti();

    putstr("waiting\n");
    while (fired == 0)
    {
        // Not a busy loop: the machine stops fetching entirely and the timer is what starts it
        // again. Every iteration after the first would be one the interrupt already ended.
        __builtin_halt();
    }

    __builtin_cli();
    *command = 0;

    putstr("the handler ran\n");
    return 0;
}

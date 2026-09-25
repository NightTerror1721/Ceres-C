# Interrupts

[← Back to index](README.md)

Every other function in a Ceres-C program runs because something called it. A handler does not. The
machine stops whatever was executing, pushes the flags and the program counter, and jumps — and when
the handler is done the interrupted code resumes as though nothing had happened.

That one difference is where every rule on this page comes from.

```c
__interrupt void term_isr(void)
{
    volatile unsigned int* in  = (volatile unsigned int*)0xFF000008;
    volatile unsigned int* out = (volatile unsigned int*)0xFF000004;
    *out = *in;                       // echo whatever arrived
}

__interrupt_vector(17, term_isr);     // 17 is the terminal's

int main(void)
{
    __builtin_sti();                  // unmask user interrupts
    __builtin_halt();                 // wait for one
    return 0;
}
```

`examples/18_interrupts.c` is the runnable version, driven by the timer instead of by typing.

## Two declarations, kept apart

`__interrupt` says **how a function is entered and left**. `__interrupt_vector` says **which number
it answers**. They are separate because CeresASM keeps them separate — `interrupt N: label` is a
statement of its own there, with no opinion about the label it names
([26-Interrupt-Vector-Binding.md](https://github.com/Krampus1721/CeresASM/blob/main/docs/26-Interrupt-Vector-Binding.md)).

Keeping them apart is what makes the feature compose:

```c
// io.c — a library publishes the handler
__interrupt void term_isr(void) { ... }

// main.c — the program that uses it picks the vector
__interrupt void term_isr(void);          // an ordinary prototype
__interrupt_vector(17, term_isr);
```

and what lets a handler written in C be bound from hand-written assembly, or the other way round —
see [C and CASM together](07-CASM-Interop.md).

## What `__interrupt` changes

| | Ordinary function | `__interrupt` handler |
| --- | --- | --- |
| Parameters | any | **none** — nobody is there to pass one |
| Return type | any | **`void`** — nobody is there to read a result |
| Called from C | yes | **no** — the vector is the only way in |
| Registers | `r0`–`r7`, `r12`, `f0`–`f7` are the caller's problem | **all of them are the handler's** |
| Last instruction | `ret` | **`iret`** |

Each row is the same fact from a different side. There is no caller, so there is nobody to hand an
argument to, nobody to hand a result back to, and — the one that costs instructions — nobody who
saved a register before the jump.

The caller/callee split in
[CeresASM's calling convention](https://github.com/Krampus1721/CeresASM/blob/main/docs/24-Calling-Convention.md)
does not apply here. `r0`–`r7`, `r12` and `f0`–`f7` are called caller-saved because a *caller* saved
them; a handler has none, so it saves them itself:

```casm
global term_isr:
    pushm 0x1FFF           // r0-r12, in one instruction
    fpushm 0x00FF          // f0-f7, in one - only when the body reaches the float bank
    ...
    fpopm 0x00FF
    popm  0x1FFF
    iret
```

The float bank goes back in one `fpushm`/`fpopm` pair, but still only when the handler can reach it
at all — which any call makes true, whatever the body itself does. The mask covers `f0`–`f7` only:
the callee-saved `f8`–`f15` never need saving, because only a *parameter* can earn a callee-saved
register and a handler has no parameters (its own locals are either forwarded away or escaped to
memory). The `r8`–`r11` half is bundled into the integer mask's `0x1FFF` for the same reason a
mask is cheap and precise there — one `pushm` is the same word however many bits are set.

Calling a handler is an error rather than a warning. `iret` pops the flags and PC the dispatcher
pushed; reached through `call` it would pop the return address as a PC and whatever sat below it as
flags.

## The numbers

The vector table has 64 entries. `__interrupt_vector` takes any constant expression, so an `enum` or
a `#define` is the readable way to write one:

```c
enum Irq { Timer = 16, Terminal = 17 };
__interrupt_vector(Timer, tick_isr);
```

| Number | Raised by | Deliverable |
| --- | --- | --- |
| 0 | reset — the entry point | never bindable; that is what `main` is |
| 1 | `trap` | always |
| 2 | illegal instruction | always |
| 3 | memory fault (a store into `.text`) | always |
| 4 | division by zero | *never dispatched* — the VM sets its Trap flag instead |
| 5 | stack overflow | always |
| 6 | alignment fault | always |
| 7 | page fault | always |
| 15 | syscall | always (nothing raises it yet) |
| 16 | the timer | only while unmasked |
| 17 | the terminal, on input | only while unmasked |
| 18–63 | unused | only while unmasked |

Four rules, all of them the linker's, are checked here so the message names your C rather than
generated assembly:

1. **The number must fold to a constant.** A variable is rejected; an enum constant or a macro is not.
2. **0 is the reset vector**, and 63 is the end of the table.
3. **The target must be `__interrupt`.** An ordinary function ends in `ret`.
4. **One binding per number.** Inside one file the error names the first binding; across objects
   that is `ceres link`'s to catch, since it is the only thing that sees them all.

## Masking: the part that is easy to forget

Interrupts **0–15 are always deliverable**. Interrupts **16–63 are not**: while the Interrupt flag
is clear, one that arrives is *dropped*, not queued. So a handler for the timer or the terminal does
nothing at all until the program says so:

```c
__builtin_sti();    // unmask 16-63
__builtin_cli();    // mask them again
__builtin_halt();   // stop fetching until an interrupt arrives
```

These three are builtins rather than library functions because there is nothing an ordinary function
could contain but the one instruction, and no header to declare them in. Each takes no arguments and
produces `void`. See [Known limitations](06-Known-Limitations.md).

`__builtin_halt()` is not a busy loop: the machine stops fetching entirely, and an interrupt is what
starts it again — the dispatcher clears the Halting flag as part of dispatch.

## What the generated `.casm` looks like

The binding emits neither code nor data, only a line the linker resolves and the loader applies
before the program's first instruction, so it sits above every section:

```casm
interrupt 17: term_isr

@text

// term_isr - echo.c:3
global term_isr:
    pushm 0x1FFF          // echo.c:3
.L0:
    la r3, -16777208      // echo.c:5
    ...
    popm 0x1FFF           // echo.c:3
    iret                  // echo.c:3
```

A running program can never patch the vector table itself — every checked write below `0x400` is
refused, on purpose, so that a stray pointer cannot corrupt interrupt dispatch. Binding at link time
and patching at load time needs no privilege model to be safe, because no *instruction* ever performs
the write.

## What is not here

- **No nesting.** Dispatch clears the Interrupt flag and `iret` restores the saved flags, so a
  handler is not itself interrupted. A handler that calls `__builtin_sti()` re-enables them early;
  nothing stops you, and nothing helps you either.
- **No `int n` from C.** Software-triggered interrupts have no builtin yet. `trap` and `int imm8`
  exist in the ISA; a `.casm` file can raise one and a C handler can answer it.
- **No priorities and no controller state.** One pending interrupt at a time, taken before the next
  instruction.
- **`at` / `r13` is not preserved.** The assembler clobbers it when it materializes a 32-bit address
  (`stv`, and a float `ldv`). The convention already says no code may rely on `r13` across an
  instruction boundary, so this costs nothing in practice — but it is a real difference from "every
  register is restored".

## Related pages

- [C and CASM together](07-CASM-Interop.md) — binding a C handler from assembly and the reverse.
- [Known limitations](06-Known-Limitations.md) — the builtins, and what `volatile` guarantees.
- [From IR to CASM](03-IR-to-CASM.md) — registers, frames and the calling convention this departs from.
- CeresASM's [Interrupts and exceptions](https://github.com/Krampus1721/CeresASM/blob/main/docs/08-Interrupts-and-Exceptions.md)
  and [Interrupt vector binding](https://github.com/Krampus1721/CeresASM/blob/main/docs/26-Interrupt-Vector-Binding.md).

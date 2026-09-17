# Known limitations

[← Back to index](README.md)

Deliberate simplifications, documented so that whoever picks this up knows where the line was drawn
and why, rather than rediscovering it.

Nothing under [`examples/`](../examples) depends on anything on this page, so the shipped programs
describe the language as it actually behaves.

> Four bugs used to be listed here — narrow loads that never sign-extended, `(char)` casts that did
> not truncate, int-to-`bool` conversions that did not normalize, and a `const`/storage-class gap in
> the parser. All four are fixed, and each is now pinned by an ordinary test rather than a
> known-failure marker. The fifth entry below is not ours.

## The assembler miscompiles the integer `neg` pseudo-instruction

This one is in **CeresASM**, not in Ceres-C, and it is worked around here rather than fixed here.
`neg rd, rs` is documented to expand to `imul rd, rs, -1`, but it assembles to `IMUL rd, r15, r0` —
the `-1` lands in the `rs` register field as `r15`, and the real source register is dropped. Every
integer negation then returns garbage.

```casm
@text
global main:
    li  r1, 3
    neg r2, r1        // disassembles as IMUL r2, r15, r0
    imul r3, r1, -1   // disassembles as IMULI r3, r1, -1  — correct
    halt
```

The float form is fine: `neg fd, fs` maps to the real `FNEG` opcode.

Ceres-C therefore writes the documented expansion out in full — `imul rd, rs, -1` — for integer
negation, and keeps using the pseudo for floats. Pinned by
`codegen / integer_negation_writes_out_the_imul_expansion_instead_of_the_neg_pseudo`, whose comment
says to check the assembler first if it ever goes red. Once the assembler is fixed, the workaround
can go.

---

## Limits

### The preprocessor has no conditionals

`#include`, `#define` (object-like), `#undef` and `#pragma once` are implemented;
`#if`/`#ifdef`/`#else`/`#endif`, macros with arguments, `#error`, `#line` and `__FILE__`/`__LINE__`
are not. `#pragma once` is what makes headers usable without them. See
[08-Preprocessor.md](08-Preprocessor.md).

### A C symbol cannot be named after a CeresASM reserved word

A C symbol keeps its own name in the generated assembly, which is what makes interoperability work
in both directions (see [07-CASM-Interop.md](07-CASM-Interop.md)). The price is that a function,
global or static local named `let`, `global`, `word`, `u32`, `align`, `assert`, `true`, `sp`, `r3`
or any of the other reserved words is a Ceres-C error asking you to rename it. Most of that list is
a C keyword anyway.

### No standard library

No `printf`, no `malloc`, no `memcpy`, no `strlen`. A program prints by storing bytes into the
terminal device's output register at `0xFF000004`; `examples/08_strings.c` writes the string
routines it needs, and `examples/interop/io.c` wraps them into something reusable. CeresASM's own
`stdlib/` currently only has `call.casm`, so there is nothing to link against yet either.

### No 64-bit width, in either bank

Ceres has no 64-bit integer register and no f64 register — not in the ISA, not in the VM. The four
C types that name one are still accepted, because refusing them turns a program that wants a wide
number into a syntax error rather than into a number, but each one caps to its 32-bit counterpart
and says so:

| Written | Is | Warning |
| --- | --- | --- |
| `long long`, `signed long long` | `long` | `'long long' is 32 bits here: this machine has no 64-bit type at all, so it is exactly 'long'` |
| `unsigned long long` | `unsigned long` | as above, naming `unsigned long` |
| `double` | `float` | as above, naming `float` |
| `long double` | `float` | as above, naming `float` |

They are **spellings**, not types of their own: `long long` and `long` are the same type, a
`double*` and a `float*` are interchangeable, and `sizeof(long long)` is 4. Modelling them as
distinct kinds would put a width in the type system that nothing below the parser can produce.
Supporting the real widths would mean software emulation — front-end and runtime work, not a type
mapping.

The warning fires at every occurrence of the spelling, including inside a `typedef`; the typedef
NAME is then an ordinary name for the capped type and says nothing further.

### No address constants in a static initializer

C lets a file-scope object be initialized with the address of another one — a string literal,
`&x`, or an array's name. All four of these are legal C and all four are refused here:

```c
char*  message = "hi";          /* error[E4006] */
char*  names[] = { "a", "b" };  /* error[E4006] */
int    g;
int*   p = &g;                  /* error[E4006] */
struct S { char* s; } s = { "x" };  /* error[E4006] */
```

The limitation is CeresASM's rather than this compiler's, and it is structural: a relocation
patches a word in `.text` and records nothing else — see
[25-Separate-Compilation.md](https://github.com/Krampus1721/CeresASM/blob/main/docs/25-Separate-Compilation.md).
An address is not known until the link, so there is no way to write one into `.data` or `.rodata`,
and the assembler refuses `let p: u32 = msg` for the same reason: *'msg' is not a constant*.

Assign it inside a function instead, where the address is an ordinary `la` like any other:

```c
char* message;
void start(void) { message = "hi"; }
```

A `char` **array** is unaffected, because the bytes are the value and no address is involved:
`char message[8] = "hi";` has always worked, at every depth of array and struct.

### No bitfields

A second layout rule to learn, and nothing needs them yet. `union` itself is supported.

Function pointers are no longer on this list. `int (*f)(int)` declares one, `f(1)` calls through it,
and a function name used as a value decays to a pointer to itself exactly as an array does — so `f`
and `&f` mean the same thing. The function type itself is real too (`typedef int Handler(int);`),
which is what makes `Handler*` and `int (*)(int)` the same type rather than two that happen to
agree. A call through a pointer becomes `call rN` — the ISA's indirect call, which was always there.
See `examples/19_function_pointers.c`.

Variadic functions are no longer on this list — `...`, `__builtin_va_list`, `__builtin_va_start`,
`__builtin_va_arg`, `__builtin_va_end` and `__builtin_va_copy` all work. What they do not come with
is a `<stdarg.h>` (the names are builtin, since there is no system include directory to find a
header in) or a `printf` to use them for, and a variadic `float` is not promoted to `double`,
because there is no `double`. See [09-Variadic-Convention.md](09-Variadic-Convention.md).

### Qualifiers and storage

`volatile` objects always retain a memory home. They are not promoted to registers, and the IR
optimizer neither forwards a preceding store into a volatile load nor removes a volatile store.
This keeps distinct device-register reads and writes observable.

That holds for an access through a pointer too: `volatile int* p` qualifies the pointee, not the
pointer, so there is no local to hang the fact on and the IR marks the individual load or store
instead (`load.word.v`). Both forms are checked by the passes that could otherwise drop such an
access, rather than being safe only for as long as those passes happen not to look at indirect
accesses.

Which side of the `*` a qualifier sits on decides what it qualifies, and both spellings parse:
`volatile int* p` is a pointer to volatile int, `int* volatile p` is a volatile pointer to ordinary
int, and `--emit-ast` prints them differently because they are different types. Either word may also
follow the type-spec (`int volatile x` is `volatile int x`), and repeating one across two positions
is still a duplicate.

`restrict` is accepted only on pointer types and is recorded in the type system. The current
optimizer does not yet use no-alias assumptions across arbitrary pointers, so the qualifier is a
checked contract rather than an unsafe speculative transformation.

`register` moves a local to the front of the queue for a machine register, ahead of everything that
did not ask. It only reorders preferences: every condition that keeps a local out of a register is a
correctness rule — a call clobbers the pool, an escaped local needs an address, a `volatile` one
needs a memory home, one wider than a register does not fit — and the keyword relaxes none of them.
"Wider than a register" is the whole of the width rule: a `char`, a `bool` and a `short` are as
eligible as an `int` is.
Where no local would have been spilled anyway it therefore says nothing new, and at `-O0` it says
nothing at all, because register allocation itself is off (see [the CLI](05-CLI.md)). It may only be
used for automatic local variables — at file scope or on a function it is rejected, as in C. Also as
in C, taking the address of one is rejected — and so is `register` on an *array*, because using an
array at all takes its address.

A *parameter* may carry it too, as in C, and it means the same thing there: this parameter is
asked for a register ahead of anything that did not ask, and its address may not be taken. It is the
only storage-class specifier a parameter accepts — `static`, `extern` and `auto` are all rejected,
because a parameter's storage is the calling convention's to decide. It is part of the declaration
and not of the type, so a prototype and the definition need not agree about it, exactly as in C.

None of the three changes which values a correct program computes — `volatile` only removes
optimizations, and `restrict` and `register` are checked contracts that the back end is free to
ignore. That is deliberate: a qualifier that silently licensed a wrong answer would be worse than
one that is merely not yet exploited.

### Interrupt delivery needs a builtin

`sti`, `cli` and `halt` are the one part of the machine no C expression reaches: there is no address
to store into and no arithmetic with the effect. They are spelled `__builtin_sti()`,
`__builtin_cli()` and `__builtin_halt()` — recognized by the parser in call position, the way the
`va_*` builtins are, since there is no header to declare them in and nothing an ordinary function
could contain but the one instruction. Each takes no arguments and produces `void`.

`__builtin_sti()` is what a program needs before a *user* interrupt (16–63) can be delivered at all;
the reserved ones (0–15) are always deliverable. `__builtin_halt()` suspends the machine until an
interrupt arrives, which is the point of binding one. See [Interrupts](10-Interrupts.md).

The `__builtin_` prefix is reserved to the implementation in C, so no existing program can be using
these names — and because they are only recognized in call position, one that uses the spelling for
a variable of its own still works.

### No integer literal suffixes

`100u`, `100L` and `1.5f` are not accepted. A literal's type comes from its form and from what it is
assigned to.

### Division by zero does not fault

The VM sets its Trap flag and leaves the destination register untouched, and nothing reads that flag
automatically. Ceres-C inherits the behaviour rather than hiding it behind an implicit check before
every division. A `--check-div-by-zero` that emitted one is a plausible future option.

### `main`'s return value is not an exit code

`ceres run` exits 0 on a clean halt and 1 on a fault, never with a value the program chose — there is
no channel from a register to a process exit code. `main` halts the machine through the system
control device, and its return value goes into `ret0` only so it stays inspectable under
`ceres debug`. Anything a program wants to report, it prints.

### Recursion depth is not checked

It does not need to be. A recursion with no base case exhausts the real stack and the VM raises
`StackOverflow`, which is a better diagnostic than anything Ceres-C could synthesize.

### Separate compilation gives up relaxation

A program built from objects is slightly larger than the same sources assembled whole: `ldv`/`stv`
keep the three words they reserved, because "within reach" is a distance an object cannot know. That
is CeresASM's own trade-off, inherited here because `--run` always goes through
`ceres asm -c` + `ceres link` — one shape for one file and for twenty, so the two cannot drift apart.

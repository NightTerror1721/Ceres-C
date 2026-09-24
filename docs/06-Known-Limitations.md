# Known limitations

[← Back to index](README.md)

Deliberate simplifications, documented so that whoever picks this up knows where the line was drawn
and why, rather than rediscovering it.

Nothing under [`examples/`](../examples) depends on anything on this page, so the shipped programs
describe the language as it actually behaves.

> Five bugs used to be listed here — narrow loads that never sign-extended, `(char)` casts that did
> not truncate, int-to-`bool` conversions that did not normalize, a `const`/storage-class gap in
> the parser, and one in CeresASM (below). All five are fixed, and each is pinned by an ordinary test
> rather than a known-failure marker.

## The integer `neg` pseudo-instruction (fixed in CeresASM)

CeresASM used to assemble `neg rd, rs` on integer registers to `IMUL rd, r15, r0` — the `-1` of its
documented expansion `imul rd, rs, -1` landed in the `rs` register field as `r15` and the source
register was dropped. CeresASM 0f827ed encodes it as the `IMULI rd, rs, -1` it was always meant to be
(pinned by its `encoding / integer_neg_is_imul_by_minus_one_of_the_source`).

Ceres-C still writes that expansion out in full — `imul rd, rs, -1` — for integer negation, and uses
the pseudo only for floats (where `neg fd, fs` is the real `FNEG` opcode). The two now assemble to the
same word, so nothing changes for a program; keeping the explicit form means Ceres-C also works with an
assembler older than the fix. Pinned by
`codegen / integer_negation_writes_out_the_imul_expansion_instead_of_the_neg_pseudo`.

---

## Limits

### The preprocessor has no `#line`

`#include`, `#define` (object-like and function-like, with variadic macros and `#`/`##`), `#undef`,
`#if`/`#elif`/`#else`/`#endif`, `#ifdef`/`#ifndef`, `#error`, `#warning`, `#pragma once` and
`#pragma warning` are all implemented, and so are `__LINE__`, `__FILE__`, `__BASE_FILE__`,
`__INCLUDE_LEVEL__` and `__COUNTER__`. Only `#line` is missing: a diagnostic's true file and line
come from a source-location map built during preprocessing instead, which is what
[08-Preprocessor.md](08-Preprocessor.md) covers under "Line numbers across an include" — an
alternative to teaching the lexer to read `#line` markers, not a gap still to close.

### A CeresASM reserved word is written with a prefix in the assembly

A C symbol keeps its own name in the generated assembly, except when that name is one CeresASM
reserves (`let`, `global`, `word`, `u32`, `align`, `assert`, `true`, `sp`, `r3`, `at`...): it is then
written `__c_` and the name (`at` is `__c_at`), and `__asm__("label")` picks any other name. Hand-written
assembly and the debugger see the prefixed name; C sees its own. See
[07-CASM-Interop.md](07-CASM-Interop.md). CeresASM itself still cannot define a routine under such a name
without the prefix.

### No standard library

No `printf`, no `malloc`, no `memcpy`, no `strlen`. A program prints by storing bytes into the
terminal device's output register at `0xFF000004`; `examples/08_strings.c` writes the string
routines it needs, and `examples/interop/io.c` wraps them into something reusable. CeresASM's own
`stdlib/` currently only has `call.casm`, so there is nothing to link against yet either.

### No 64-bit register, so a 64-bit value is a pair of words

Ceres has no 64-bit integer register and no f64 register — not in the ISA, not in the VM.

The two C types that name a wide **integer** are real types, not spellings: `long long` and
`unsigned long long` are 8 bytes with alignment 8, so `sizeof(long long)` is 8, a struct field of
one is laid out for a real 64-bit object, and an `ll`/`LL` literal suffix gives a literal the 64-bit
type. A value is lowered as an **addressed pair of 32-bit words** — the low word at offset 0, the
high word at offset 4, little-endian — exactly the way a struct is represented, so the whole back
end handles it with no new IR opcode. That covers:

- **load/store/copy/assignment**, through a variable, a struct field, an array element or a global;
- **`+`, `-`**, with the carry/borrow crossing the word boundary (`Cmp` supplies the flag — there is
  no `ADDC` in the IR), and the bitwise **`&`, `|`, `^`, `~`** and unary **`-`**;
- **`*`, `/`, `%`**: a product composes from the 32-bit `mul` and the unsigned multiply-high
  (`MULH`), and a division or remainder calls the compiler's own `__cc_div64` — a restoring
  shift-subtract loop the back end emits once, at the end of `@text`, only when a site asks for it.
  The quotient and the remainder share the call, signed and unsigned both;
- **`<<`, `>>`** by 0..63, including the crossing at 32 (the low word comes from the high one, or is
  zeroed on a left shift) and an arithmetic fill for a signed `>>`;
- **comparisons** (`==`, `!=`, `<`, `<=`, `>`, `>=`), signed or unsigned, comparing the high words
  first and the low words unsigned, and against a `float` operand via a whole-value float conversion;
- **`int`↔`long long`** (sign/zero extension, truncation to the low word) and **`float`↔`long long`**:
  a conversion goes sixteen bits at a time (as the STDLIB's `ns64_to_float` does), so a value that
  does not fit 32 bits keeps its high half. `long long`→`float` is within one ULP of correctly
  rounded (the machine's f32 cannot always do better with a 64-bit source); `float`→`long long` is
  exact for every value in range;
- `++`/`--`, a `?:` whose result is 64-bit, a 64-bit `if`/`while` condition;
- **crossing a function boundary by value** (F3.4): a wide parameter arrives in two consecutive
  argument registers (or two outgoing stack words, once the four are spent), a wide argument is passed
  as those two words, and a wide result comes back in `ret0`/`ret1`. A function with a wide parameter
  or return type is still **not inlined** and a call that passes or returns a wide value is not turned
  into a **tail call** — those two optimizations deliberately refuse the shape rather than model the
  pair.

```c
long long  a = 0x0000000100000002LL;  /* fine */
a + 1;  a * 2;  a / 3;  a << 40;  (float)a;  (long long)1.5f;  /* fine */
long long  f(long long v);           /* fine: v arrives in r0/r1, the result returns in r0/r1 */
```

Still refused with `E5002` rather than silently truncated, because 64 bits has no encoding there:

- a 64-bit `switch` discriminant (F9);
- a 64-bit operand to a one-instruction machine builtin (there is no 64-bit form of it).

A decimal literal with an `ll`/`LL` suffix whose value does not fit a signed `long long` (e.g.
`18446744073709551615LL`) is out of range in C; here it warns (`W0015`) and keeps its 64-bit bit
pattern, so it reads as `-1`. An `ULL` literal of the full range does not warn.

A zero **divisor** in a 64-bit `/` or `%` is undefined in C, and the emitted routine stores zero
rather than looping — the 32-bit instructions trap instead (see the division-by-zero note below).

The two types that name a wide **float** are still capped, because there is no f64 register to give
them: `double` and `long double` are spellings of `float`, and the parser says so:

| Written | Is | Warning |
| --- | --- | --- |
| `double` | `float` | `'double' is 32 bits here: this machine has no 64-bit floating-point type, so it is exactly 'float'` |
| `long double` | `float` | as above, naming `float` |

The warning fires at every occurrence of the spelling, including inside a `typedef`; the typedef
NAME is then an ordinary name for the capped type and says nothing further.

### Arithmetic in a static initializer

A global or a `static` is initialized from a constant expression, and arithmetic on constants is one: `static int n = 2 + 3;`,
`{ 64, 4 * 20 }` in a table, `FLAG_A | FLAG_B` with enumerators, `sizeof(table) / sizeof(table[0])`, `LIMIT > 4 ? 1 : 2`, `1u << 31`.
Sema records each value on the expression (`Expr::constantValue`) and code generation writes it into the image at the
declared width. What still is not a constant is a variable, a call, a comma, a division by zero, and a
floating-point expression (`1.0f / 3`); an integer constant converts to a `float` target (`float f = 2 * 3;`).
An enumerator or `sizeof` as an array *size* is still refused (E2031): that path takes only literals.

### No address constant with an offset in a static initializer

C lets a file-scope object be initialized with the address of another object — a string literal,
`&x`, or an array's or function's name. All of these are legal C and all of them work here now:

```c
char*  message = "hi";                /* a string literal's address */
char*  names[] = { "a", "b" };        /* an array of them */
int    g;
int*   p = &g;                        /* &x */
struct S { char* s; } s = { "x" };    /* a pointer field */
```

CeresASM relocations patch a word in `.data` and `.rodata`, not just `.text` — see
[25-Separate-Compilation.md](https://github.com/Krampus1721/CeresASM/blob/main/docs/25-Separate-Compilation.md) —
so the address is left for the link to fill in, exactly like an instruction operand's is.

The object may itself be `static`, at file scope or in a block - `static const struct S s = { 3, cells };`
with `cells` a `static` array - since a `static` object has a fixed address too. An automatic local or a
parameter does not, so `static int* p = &local;` is still refused (`E3017`).

A cast leaves a constant a constant: `(void*)0` - which is what `NULL` expands to, so `{ "a", NULL, "c" }`
is a valid table - and `(char*)table` or `(float)3`.

A **null pointer constant** - `0`, or `(void*)0` / `NULL` - converts to any pointer type, function pointers
included: `handler = NULL;`, `static void (*table[2])(void) = { f, NULL };` and `c ? handler : NULL` all work. A
`void*` that is not that constant, or an address other than zero, still does not become a function pointer,
since calling through a mismatched signature puts the wrong arguments in the wrong registers.

What is still refused is an address **with an offset**: `&a[i]` is an address constant in C, but it is
the address of `a` plus a displacement, and there is no way to spell that displacement into the
initializer. Write the base pointer and add the offset at run time instead:

```c
int a[4];
int* base = a;              /* fine: a's address, no offset */
int* fourth = &a[3];        /* error[E4006]: a's address plus an offset */
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

`restrict` is accepted only on pointer types and is recorded in the type system. The optimizer uses it
for one no-alias assumption: a load through a `restrict` pointer forwards from the most recent store
through that *same* pointer, even when stores through other pointers sit in between (because restrict
is the promise that nothing else aliases the pointee). Only the direct `*p` form is recognized —
pointer arithmetic (`p[i]`) has a computed address that is not traced back to the pointer — and any
store through an ordinary (non-`restrict`) pointer, or any call, is still treated as possibly aliasing,
so the qualifier remains a checked contract first and a speculative transformation second.

`register` moves a local to the front of the queue for a machine register, ahead of everything that
did not ask. It only reorders preferences: every condition that keeps a local out of a register is a
correctness rule — a call clobbers the caller-saved pool (a local that must survive one competes for
the callee-saved half instead), an escaped local needs an address, a `volatile` one
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

### Division by zero does not fault

The VM sets its Trap flag and leaves the destination register untouched, and nothing reads that flag
automatically. Ceres-C inherits the behaviour rather than hiding it behind an implicit check before
every division. A `--check-div-by-zero` that emitted one is a plausible future option.

### `main`'s return value is the exit status

`ceres run` exits with what `main` returned (its low eight bits, as with a POSIX status) and with 1 on a
fault. Falling off the end of `main`, or a `void main`, is status 0. This needs a CeresASM that has the
status byte in the system control device's command word; an older VM reads only the low byte of that
word, so the program still stops cleanly and the status is simply 0.

A unit that declares `void exit(int)` (any that includes a C library's `<stdlib.h>` or `<stdio.h>`)
ends `main` by calling it: `return n;` is `exit(n)`, so the handlers registered with `atexit` run and
open files are flushed. A unit that does not declare it writes the status to the control device itself.

### Recursion depth is not checked

It does not need to be. A recursion with no base case exhausts the real stack and the VM raises
`StackOverflow`, which is a better diagnostic than anything Ceres-C could synthesize.

### Separate compilation gives up relaxation

A program built from objects is slightly larger than the same sources assembled whole: `ldv`/`stv`
keep the three words they reserved, because "within reach" is a distance an object cannot know. That
is CeresASM's own trade-off, inherited here because `--run` always goes through
`ceres asm -c` + `ceres link` — one shape for one file and for twenty, so the two cannot drift apart.

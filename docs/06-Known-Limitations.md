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

### No `double`

The VM has no double-precision floating point at all. Supporting `double` would mean software
emulation — real front-end and runtime work, not a type mapping. `float` (f32) is fully supported
and has its own register bank.

### No `union`, bitfields, function pointers or varargs

Each is a mechanism with no user yet. Function pointers are the most nearly free of the four: the
ISA already has indirect calls.

### No `volatile`

Which matters here more than it usually would: a device register read twice really should be read
twice, and nothing stops the optimizer forwarding the first read to the second. `libs/ir`'s
optimizer treats a `Load` as impure for exactly this reason, but that is a blunt instrument rather
than the qualifier.

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

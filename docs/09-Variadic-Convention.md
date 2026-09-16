# Variadic functions

[← Back to index](README.md)

A function whose parameter list ends in `...` accepts arguments its declaration does not describe:

```c
int sum(int count, ...);
```

`count` is a **fixed** parameter. Everything passed beyond it is the **variadic tail**. This page is
the contract between the two sides of such a call — what the caller puts where, and where the callee
looks for it.

## The rule

> **Fixed arguments follow the ordinary calling convention. Every argument in the variadic tail is
> passed in the outgoing stack area, whichever bank it would otherwise have used and however many
> argument registers are still free.**

The ordinary rule is
[CeresASM's own calling convention](https://github.com/Krampus1721/CeresASM/blob/main/docs/24-Calling-Convention.md):
the first four integer or pointer arguments go in `r0`–`r3`, the first four floats in `f0`–`f3`, counted separately, and
anything past its own bank's fourth spot goes to the next outgoing stack word. The tail simply skips
the register half of that rule.

So for `sum(4, 10, 20, 30, 40)`:

| Argument | Where it goes |
| --- | --- |
| `4` (fixed) | `r0` |
| `10` | outgoing word 0 — the callee's `[fp + 8]` |
| `20` | outgoing word 1 — `[fp + 12]` |
| `30` | outgoing word 2 — `[fp + 16]` |
| `40` | outgoing word 3 — `[fp + 20]` |

`r1`–`r3` stay untouched, and so do `f0`–`f3`.

### Why the tail is not passed in registers

Because the callee has to be able to *find* it. A variadic callee does not know the types of its tail
— that is the whole point — so at the moment it runs it cannot reconstruct which arguments the
caller put in `r2` and which in `f1`. An argument sitting in a register is indistinguishable from a
fixed one.

The usual answer is a **register save area**: the callee spills `r0`–`r3` and `f0`–`f3` into its own
frame on entry, and `va_arg` indexes into that area with a per-bank cursor, the way the x86-64 SysV
ABI does. That works, but it costs eight stores in the prologue of every variadic function and
three cursors in every `va_list`.

Putting the tail on the stack instead makes it one contiguous run of words at a statically known
offset, which reduces `va_list` to a single pointer and `va_arg` to a load and an add. AArch64 on
Apple platforms makes the same trade for the same reason. The cost is that a variadic call is
slightly slower than an ordinary one — which is the right way round, since ordinary calls are the
common case and pay nothing.

### Where the tail starts

At the first incoming stack word the fixed parameters did not already take. Both sides compute it
the same way, from the fixed parameter list alone:

```c
int f(int a, ...);                              /* a is in r0  -> tail starts at [fp + 8]  */
int g(int a, int b, int c, int d, int e, ...);  /* e is on the stack -> tail at [fp + 12]  */
```

Incoming stack arguments begin at `[fp + 8]`: `[fp + 0]` holds the saved frame pointer and
`[fp + 4]` the return address.

A variadic function therefore **always has a frame**. It addresses its tail through `fp`, so the
frameless-leaf optimization does not apply to one however little else it would have put in a frame.

### What may travel through `...`

Scalars only — integers, pointers and floats. A `struct` or `union` argument is rejected at compile
time: it would be passed as a hidden pointer to a caller-owned copy, and nothing in that convention
tells the callee how big the copy is, so `va_arg` could never read it back.

## Default argument promotions

C promotes anything narrower than `int` to `int` before it enters the tail. That happens here too,
and it happens for free: the IR keeps a value of narrow type in its already-narrowed representation
at all times, so the word the caller stores *is* the promoted `int` — a `char` holding `-1` arrives
as the word `0xFFFFFFFF`, and an `unsigned char` holding `200` arrives as `200`.

**`float` is the one deliberate deviation from C.** Standard C promotes `float` to `double` in a
variadic call. Ceres has no `f64` at any level — not in the ISA, not in the VM, not in this
compiler's type system — so there is nothing for that promotion to target. A `float` travels as the
`f32` it already is, and is read back with `va_arg(ap, float)`.

This is a real incompatibility and worth stating plainly: C code written against a `double`-based
`printf` will not port unchanged.

## `va_list` and the four operations

`va_list`, `va_start`, `va_arg`, `va_end` and `va_copy` are **built into the compiler**. There is no
`<stdarg.h>` to include, and no system include directory to find one in — the names are simply
known:

```c
int sum(int count, ...)
{
    va_list ap;
    int total = 0;
    int i;

    va_start(ap, count);
    for (i = 0; i < count; i = i + 1)
        total = total + va_arg(ap, int);
    va_end(ap);
    return total;
}
```

| Form | What it means |
| --- | --- |
| `va_list` | A cursor into the caller's frame. It is a `char*`, so it assigns, copies and passes like any other pointer. |
| `va_start(ap, last)` | Points `ap` at the first argument of the tail. `last` must name the **last fixed parameter**. |
| `va_arg(ap, T)` | Reads the next argument as `T` and advances `ap` one word. |
| `va_end(ap)` | Nothing — a `va_list` owns no resource. Write it anyway; it is free and it is what C expects. |
| `va_copy(dst, src)` | Copies the cursor, so the tail can be walked twice. |

They are recognized as *syntax*, not as calls — `va_arg`'s second operand is a type name, which no
ordinary call could express, and all four write through the `va_list` the caller named rather than
through a copy of it. The names are not reserved: a program that uses `va_arg` as an ordinary
variable keeps working, because only `va_arg(` is treated as the builtin.

### What the compiler checks

- `va_start` is only allowed inside a function declared with `...`, and its second operand must name
  that function's last fixed parameter — naming any other one would describe a different starting
  point than the one `va_start` actually produces.
- `va_arg`'s type must be a 4-byte scalar. `va_arg(ap, char)` is rejected rather than silently
  decoding a word that holds an `int`: after the default argument promotions, no `char` was ever
  passed. Read it as `int` and convert.
- A call to a variadic function must supply all of its fixed arguments, which are type-checked as
  usual; anything beyond them is unchecked by definition.
- A prototype and a definition must agree about `...`. It is part of the signature, not a detail of
  one declaration — every call site was already compiled against it.

### Passing a `va_list` on

Because a `va_list` is an ordinary pointer, the `vprintf` pattern works: a variadic entry point can
hand its cursor to a non-variadic worker.

```c
int vsum(int count, va_list ap)
{
    int total = 0;
    int i;
    for (i = 0; i < count; i = i + 1)
        total = total + va_arg(ap, int);
    return total;
}

int sum(int count, ...)
{
    va_list ap;
    int r;
    va_start(ap, count);
    r = vsum(count, ap);
    va_end(ap);
    return r;
}
```

The worker advances its own copy of the cursor, not the caller's — the same as C, where a `va_list`
passed by value may be consumed but the caller must not then use its own.

## Interop with hand-written CASM

A variadic function declared in C and implemented in `.casm` (or the other way round) follows exactly
the table at the top of this page. A CASM implementation of `int sum(int count, ...)` reads `count`
from `r0` and the tail from `[fp + 8]` upward, one word each, and needs a real `enter` to have an
`fp` to read them through. See [07-CASM-Interop.md](07-CASM-Interop.md).

## What is not supported

- **No `<stdarg.h>`.** The names are builtin; there is no header to include.
- **No `double`.** See the promotion note above.
- **No aggregates through `...`.** Rejected at compile time.
- **No `printf`.** This compiler ships no standard library; variadic functions are the mechanism a
  `printf` would be written *with*, not a `printf` itself.
- **A variadic function is never inlined.** Its arguments are not described by its parameter list,
  and its body reads them out of the caller's frame — neither survives being spliced into a
  different frame.

## Related pages

- [02-Grammar.md](02-Grammar.md) — where `...` sits in the grammar.
- [03-IR-to-CASM.md](03-IR-to-CASM.md) — the IR opcode behind `va_start`, and the ordinary calling
  convention this one extends.
- [06-Known-Limitations.md](06-Known-Limitations.md) — the rest of what this version leaves out.

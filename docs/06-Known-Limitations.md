# Known limitations

[← Back to index](README.md)

Two kinds of thing are on this page, and they are kept apart on purpose:

- **Bugs** — the compiler produces wrong code for a program it accepts. Each one has a reproducer
  and a `TEST_KNOWN_FAILURE` in `tests/e2e` that will turn the build **red** the day the behaviour
  becomes correct, which is the signal to delete both the marker and the entry here.
- **Limits** — a deliberate simplification. Documented so that whoever picks this up knows where the
  line was drawn and why, rather than rediscovering it.

Nothing under [`examples/`](../examples) depends on anything on this page, so the shipped programs
describe the language as it actually behaves.

---

## Bugs

### Narrow loads never sign-extend

`ldrb` and `ldrh` are unsigned, and nothing extends the sign afterwards, so a negative `signed char`
or `short` read back **from memory** comes back as a large positive number. The two optimization
levels disagree, which is what makes it a bug rather than a documented limit: at `-O0` the value
round-trips through a one-byte frame slot and loses its sign, while at `-O1`/`-O2` it may stay in a
register and keep it.

```c
int opaque(int v) { return v; }
int main(void)
{
    char* term = (char*)0xFF000004;
    signed char small = (signed char)opaque(-3);
    *term = 48 + (small < 0);   // '1' is correct; -O0 prints '0'
    return 0;
}
```

The architecture plan's IR→CASM table says every load is unsigned "en v1", which was consistent with
its original decision that `char`/`short` were always unsigned. That decision was superseded on
2026-09-14 — signed narrow types are in scope now — but the table was never revisited, so the back
end still implements the older rule.

Pinned by `e2e / a_negative_signed_char_read_back_from_memory_keeps_its_sign`.

### Casts to a narrower integer type are no-ops

`(char)x` and `(short)x` do not truncate. The conversion is dropped rather than lowered, so the
value keeps all 32 bits — the same way at every optimization level.

```c
int opaque(int v) { return v; }
int main(void)
{
    char* term = (char*)0xFF000004;
    int wide = opaque(0x101);               // 257; its low byte is 1
    *term = 48 + ((int)(char)wide == 1);    // '1' is correct; prints '0'
    return 0;
}
```

Pinned by `e2e / a_cast_to_a_narrower_integer_type_truncates`.

### Converting an int to `bool` does not normalize

In C, any non-zero value converts to `true`, i.e. to 1. Here the value is carried across unchanged,
so a `bool` can hold 42.

```c
int opaque(int v) { return v; }
int main(void)
{
    char* term = (char*)0xFF000004;
    bool flag = opaque(42);
    *term = 48 + flag;   // '1' is correct; prints 'Z' (48 + 42)
    return 0;
}
```

Pinned by `e2e / converting_an_int_to_bool_normalizes_to_zero_or_one`.

### The assembler miscompiles the integer `neg` pseudo-instruction

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

### `const` and the storage-class specifiers are not parsed

`const`, `static`, `extern`, `auto` and `inline` are in the grammar
([02-Grammar.md](02-Grammar.md)) and are recognized by the lexer, but the parser does not accept
them anywhere yet:

```c
const int limit = 3;            // error: expected a declaration but found 'const'
static int counter;             // error: expected a declaration but found 'static'
void f(const char* s) { }       // error: expected type name but found 'const'
```

The type system already carries a `const` flag, so this is a parser gap rather than a design
decision. Nothing else depends on it: a global is visible across the whole translation unit anyway,
since there is only ever one.

### No preprocessor

There is no `#include`, no `#define` and no conditional compilation. A preprocessor would be a
text-to-text pass running before the lexer, not a feature of it. This is why every example under
`examples/` defines its own `put`/`putstr`/`putint` instead of sharing them.

### No standard library

No `printf`, no `malloc`, no `memcpy`, no `strlen`. A program prints by storing bytes into the
terminal device's output register at `0xFF000004`; `examples/08_strings.c` writes the string
routines it needs. CeresASM's own `stdlib/` currently only has `call.casm`, so there is nothing to
link against yet either.

### No `double`

The VM has no double-precision floating point at all. Supporting `double` would mean software
emulation — real front-end and runtime work, not a type mapping. `float` (f32) is fully supported
and has its own register bank.

### No `union`, bitfields, function pointers or varargs

Each is a mechanism with no user yet. Function pointers are the most nearly free of the four: the
ISA already has indirect calls.

### No integer literal suffixes

`100u`, `100L` and `1.5f` are not accepted. A literal's type comes from its form and from what it is
assigned to.

### No conditional operator

`?:` is absent from the subset. `if`/`else` covers it.

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

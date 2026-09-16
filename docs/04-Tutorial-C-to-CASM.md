# From C to CASM, one program at a time

[← Back to index](README.md)

This follows [`examples/15_suma_array.c`](../examples/15_suma_array.c) through every stage of the
compiler. Every listing below is the compiler's **actual output**, copied from a run — not a
sketch of what it might produce. You can regenerate all of it yourself with the commands shown.

## The program

```c
int suma_array(int* arr, int n)
{
    int total = 0;
    for (int i = 0; i < n; i++)
    {
        total = total + arr[i];
    }
    return total;
}
```

A loop, an indexed load, and an array whose address crossed a call boundary to get here. Small, but
it touches almost everything the back end has to get right.

## Stage 1 — tokens

The lexer turns the character buffer into a flat stream. It knows nothing about grammar: it does not
know that `for` needs parentheses, only that `for` is the keyword `For`.

```
Int Identifier(suma_array) LParen Int Star Identifier(arr) Comma
Int Identifier(n) RParen LBrace
Int Identifier(total) Eq IntLiteral(0) Semi
For LParen Int Identifier(i) Eq IntLiteral(0) Semi
Identifier(i) Lt Identifier(n) Semi Identifier(i) PlusPlus RParen
LBrace Identifier(total) Eq Identifier(total) Plus Identifier(arr)
LBracket Identifier(i) RBracket Semi RBrace RBrace
```

Operators are matched by maximal munch: `>>=` is tried before `>>`, which is tried before `>`. An
unrecognized character does not stop the lexer — it reports a diagnostic, emits one `Invalid` token
and moves on, so a stray character does not cost you the rest of the file.

## Stage 2 and 3 — syntax tree, then types

```sh
ceresc examples/15_suma_array.c --emit-ast
```

The parser builds the tree; sema then walks it and annotates it. After sema, every expression knows
its type, every name is resolved to a symbol, and every local has a slot index. That is also where
`i < n` learns it is a *signed* comparison — which is what later picks `ifls` over `ifbl`.

If sema finishes with even one error, the driver stops there. There is no point generating code for
a program that does not type-check.

## Stage 4 — the intermediate representation

```sh
ceresc examples/15_suma_array.c -O0 --emit-ir
```

```
function suma_array(params=2, locals=4) {
L0:
  %0 = const 0
  %1 = &local 2
  store.word [%1], %0
  %2 = const 0
  %3 = &local 3
  store.word [%3], %2
  jmp L1
L1:
  %4 = &local 3
  %5 = load.word [%4]
  %6 = &local 1
  %7 = load.word [%6]
  %8 = cmp.lt %5, %7
  %9 = const 0
  br.ne %8, %9, L2, L4
L2:
  %10 = &local 2
  %11 = load.word [%10]
  %12 = &local 0
  %13 = load.word [%12]
  %14 = &local 3
  %15 = load.word [%14]
  %16 = const 4
  %17 = mul.u %15, %16
  %18 = add.u %13, %17
  %19 = load.word [%18]
  %20 = add %11, %19
  %21 = &local 2
  store.word [%21], %20
  jmp L3
L3:
  %22 = &local 3
  %23 = load.word [%22]
  %24 = const 1
  %25 = add %23, %24
  store.word [%22], %25
  jmp L1
L4:
  %26 = &local 2
  %27 = load.word [%26]
  ret %27
}
```

Four blocks, and the `for` loop's shape is now explicit rather than syntactic:

- `L0` is the initializer — `total = 0`, `i = 0` — then an unconditional jump into the test.
- `L1` is the test. `i >= n` leaves for `L4`; otherwise the body.
- `L2` is the body. `%17 = mul.u %15, 4` is the index scaling, `%18` the element's address, `%19`
  the load. That is `arr[i]`.
- `L3` is the increment, which is where a `continue` would jump to — not to `L1`.
- `L4` loads `total` one last time and returns it.

Locals are numbered rather than named: `&local 0` and `&local 1` are the parameters `arr` and `n`,
`&local 2` is `total`, `&local 3` is `i`. Nothing here mentions a machine register. That is the
point of the layer.

## Stage 5 — CASM, unoptimized

```sh
ceresc examples/15_suma_array.c -O0 -o suma.casm
```

At `-O0` there is no register allocation at all: every value gets its own permanent frame field, and
every single use goes to memory and back. It is verbose on purpose — there is no analysis to be
wrong, which is what makes it the thing to bisect against.

```casm
struct __frame_suma_array
    slot0: u32
    slot1: u32
    ...
    slot31: u32
endstruct
cc_suma_array:
    enter __frame_suma_array // examples/15_suma_array.c:17
    str [sp + __frame_suma_array.slot0], r0 // examples/15_suma_array.c:17
    str [sp + __frame_suma_array.slot1], r1 // examples/15_suma_array.c:17
```

Thirty-two fields for a seven-line function, and the first thing it does is spill both incoming
argument registers into two of them. The test block shows what that costs:

```casm
.L1:
    la r4, [sp + __frame_suma_array.slot3] // examples/15_suma_array.c:20
    str [sp + __frame_suma_array.slot8], r4 //   %4 = &local 3
    ldr r4, [sp + __frame_suma_array.slot8]
    ldr r5, [r4]                             //   %5 = load.word [%4]
    str [sp + __frame_suma_array.slot9], r5
    la r4, [sp + __frame_suma_array.slot1]   //   %6 = &local 1
    str [sp + __frame_suma_array.slot10], r4
    ldr r4, [sp + __frame_suma_array.slot10]
    ldr r5, [r4]                             //   %7 = load.word [%6]
    str [sp + __frame_suma_array.slot11], r5
    ldr r4, [sp + __frame_suma_array.slot9]
    ldr r5, [sp + __frame_suma_array.slot11]
    ifls r4, r5, .cmp0_true                  //   %8 = cmp.lt %5, %7
    li r4, 0
    jp .cmp0_end
.cmp0_true:
    li r4, 1
.cmp0_end:
    str [sp + __frame_suma_array.slot12], r4
```

*(Trimmed: the source-line comments after the first two are replaced with the IR line each group
comes from, and only this one block of five is shown.)*

Every IR instruction becomes an obviously correct handful of CASM ones. `cmp.lt` producing a *value*
is the four-instruction `setcc` synthesis from
[03-IR-to-CASM.md](03-IR-to-CASM.md#there-is-no-setcc) — the ISA has no instruction that turns a
condition into a 0 or a 1.

## Stage 6 — CASM, optimized

```sh
ceresc examples/15_suma_array.c -O2 -o suma-opt.casm
```

```casm
struct __frame_suma_array
    slot0: u32
endstruct
cc_suma_array:
    enter __frame_suma_array // examples/15_suma_array.c:17
.L0:
    li r3, 0              // examples/15_suma_array.c:19
    mov r6, r3            // examples/15_suma_array.c:19
    li r3, 0              // examples/15_suma_array.c:20
    mov r7, r3            // examples/15_suma_array.c:20
.L1:
    mov r3, r7            // examples/15_suma_array.c:20
    mov r2, r1            // examples/15_suma_array.c:20
    ifge r3, r2, .L4      // examples/15_suma_array.c:20
.L2:
    mov r3, r6            // examples/15_suma_array.c:22
    mov r2, r0            // examples/15_suma_array.c:22
    mov r12, r7           // examples/15_suma_array.c:22
    mul r4, r12, 4        // examples/15_suma_array.c:22
    str [sp + __frame_suma_array.slot0], r4 // examples/15_suma_array.c:22
    ldr r5, [sp + __frame_suma_array.slot0] // examples/15_suma_array.c:22
    ldr r2, [r2 + r5]     // examples/15_suma_array.c:22
    add r12, r3, r2       // examples/15_suma_array.c:22
    mov r6, r12           // examples/15_suma_array.c:22
.L3:
    mov r3, r7            // examples/15_suma_array.c:20
    add r12, r3, 1        // examples/15_suma_array.c:20
    mov r7, r12           // examples/15_suma_array.c:20
    jp .L1                // examples/15_suma_array.c:20
.L4:
    mov r3, r6            // examples/15_suma_array.c:24
    mov r0, r3            // examples/15_suma_array.c:24
    leave                 // examples/15_suma_array.c:24
    ret                   // examples/15_suma_array.c:24
```

Thirty-one fields of frame are gone. `suma_array` calls nothing, so its locals are allowed to stay
in registers for the whole function: `total` lives in `r6` and `i` in `r7`, and the parameters stay
in the registers they arrived in. The whole loop is one indexed load and two adds.

The comparison is also no longer a value. `%8 = cmp.lt` feeding a `br.ne` collapses into a single
`ifge r3, r2, .L4` — the `setcc` synthesis was only ever needed because the *value* was.

Both versions compute the same thing, which is not a claim: `tests/examples` runs every example at
`-O0`, `-O1` **and** `-O2` on every build and requires all three to print the same bytes.

## Stage 7 — assemble and run

```sh
ceres asm suma.casm -o suma.cres
ceres run suma.cres
100
```

Or in one step, with Ceres-C launching both as subprocesses exactly as you would from a terminal:

```sh
ceresc examples/15_suma_array.c --run
100
```

The architecture plan's original version of this program ended with `return suma_array(datos, 4);`
and said the process would exit with code 100. That did not survive contact with the real VM:
`ceres run` exits 0 on a clean halt and 1 on a fault, and there is no channel from a register to an
exit code at all. So the example prints the total instead — which is the only way any program here
reports anything, and the reason every file under `examples/` starts by defining its own `put`.

## Where to look next

- [02-Grammar.md](02-Grammar.md) — what else the subset accepts.
- [03-IR-to-CASM.md](03-IR-to-CASM.md) — the full instruction mapping and the register rules.
- [`examples/`](../examples) — fifteen more programs, each with the output it must produce.

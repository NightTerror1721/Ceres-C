# From IR to CASM

[← Back to index](README.md)

Between the type-checked syntax tree and the generated assembly sits one more layer: a
three-address intermediate representation. It is what `--emit-ir` prints, and it exists for three
reasons — it separates *what to compute* from *which register holds it*, it is observable, and it
gives optimizations somewhere to live that is not the back end.

There is no SSA form and no dominator tree. An `IrFunction` is a list of basic blocks, each a
straight run of instructions ending in a jump or a return.

## The IR instruction set

| Opcode | Form | Meaning |
| --- | --- | --- |
| `Const` | `%t = const V` | A literal. |
| `BinOp` | `%t = op %a, %b` | Add/Sub/Mul/Div/Mod/And/Or/Xor/Shl/Shr/Sar, each with a signed and an unsigned variant. |
| `UnOp` | `%t = op %a` | Neg, Not, LogicalNot, IntToFloat, FloatToInt. |
| `Cmp` | `%t = cmp.eq/ne/lt/le/gt/ge %a, %b` | Produces 0 or 1 — see the note on `setcc` below. |
| `Copy` | `%t = %a` | An alias; usually disappears in the back end. |
| `Narrow` | `%t = narrow.byte/half[.u] %a` | An int-to-int width change: truncate, then extend by the target type's own signedness. |
| `ToBool` | `%t = tobool %a` | C's int-to-bool rule: zero stays zero, anything else becomes one. |
| `FrameAddr` | `%t = &local N` | The address of a local or parameter's frame slot. |
| `GlobalAddr` | `%t = &global "name"` | The address of a global, or of a string literal. |
| `Load` | `%t = load.<size>[.s] [%addr]` | `size` ∈ {byte, half, word}; `.s` means the loaded type is signed. |
| `Store` | `store.<size> [%addr], %v` | |
| `Param` | `param %v`, or `param.var %v` | Queues one outgoing argument. `.var` marks one in the callee's variadic tail, which is always passed on the stack. |
| `Call` | `%t = call f, N` | `N` is how many `Param`s were queued. A call through a function pointer prints the temporary it jumps through instead of a name: `%t = call %a, N`. |
| `VaStart` | `%t = va_start` | The address of this function's own variadic tail. No operand: the back end resolves it from the signature. |
| `Jump` | `jmp L` | |
| `CondJump` | `br.<pred> %a, %b, Ltrue, Lfalse` | One of the six predicates, signed or unsigned. |
| `Return` | `ret %v?` | |

## The instruction mapping

Verified against CeresASM's `docs/05-Instruction-Set.md` and `docs/06-Pseudo-Instructions.md`.

| IR | CASM (signed / unsigned) | Notes |
| --- | --- | --- |
| `BinOp Add`/`Sub` | `add` / `sub` (`addi`/`subi` with a constant operand) | The ISA has no signed/unsigned distinction for these. |
| `BinOp Mul` | `imul` (signed) · `mul` (unsigned) | Both keep the low 32 bits, so a signed overflow wraps silently, as in C. |
| `BinOp Div`/`Mod` | `idiv`/`imod` · `div`/`mod` | Division by zero does **not** abort — see below. |
| `BinOp And`/`Or`/`Xor` | `and` / `or` / `xor` | |
| `BinOp Shl` | `shl` | |
| `BinOp Shr` | `sar` (signed, arithmetic) · `shr` (unsigned, logical) | A real ISA distinction, not cosmetic. |
| `UnOp Neg` (int) | `imul rd, rs, -1` | The documented expansion of the `neg` pseudo, written out — see [06-Known-Limitations.md](06-Known-Limitations.md). |
| `UnOp Neg` (float) | `neg fd, fs` → `FNEG` | |
| `UnOp Not` | `not rd, rs` | Register form only; no immediate. |
| `Narrow` (signed) | `sxtb` / `sxth` | One instruction either way. |
| `Narrow` (unsigned) | `and rd, rs, 255` / `and rd, rs, 65535` | `and`'s immediate is zero-extended, so 0xFFFF arrives intact. |
| `ToBool` | `min rd, rs, 1` | Unsigned `min`, so the result is 0 only when the operand is. One instruction, no branch. |
| `Cmp eq`/`ne` | `ifeq` / `ifne` | Same for signed and unsigned. |
| `Cmp lt/le/gt/ge` | `ifls`/`ifle`/`ifgr`/`ifge` (signed) · `ifbl`/`ifbe`/`ifab`/`ifae` (unsigned, pointers, sizes) | |
| `Load` byte/half/word | `ldrb`/`ldrh` (unsigned) · `ldrsb`/`ldrsh` (signed) · `ldr` | The pair comes from the loaded TYPE. A word load fills the register, so it has no such choice. `[base + N]` with a constant offset, `[base + rIdx]` when the offset is in a register. |
| `Store` byte/half/word | `strb` / `strh` / `str` | Same two addressing forms. |
| `FrameAddr` | `la rd, [sp + Frame.slotN]`, or nothing at all if the local lives in a register | |
| `GlobalAddr` | `la rd, symbol` | String literals become `@rodata` entries (`let __ccstr0: u8[15] = "ceres compiler"`). |
| `Param` / `Call` | first four of each bank in `arg0`–`arg3`/`f0`–`f3`, the rest in the outgoing area, then `call f` | A `param.var` skips the register half of that rule entirely. An indirect call materializes its target into `r12` and emits `call r12` — same mnemonic, register operand, which is how the assembler selects `CALLR`. `r12` is free at that point by the same rule that makes it allocatable: nothing holds an allocatable register across a call. |
| `VaStart` | `la rd, [fp + 8 + 4*S]` | `S` is how many incoming stack words this function's own fixed parameters took. |
| `Jump` / `CondJump` | `jp` / the `ifXX` above | |
| `Return` | `mov ret0, %v` then `leave`/`ret` | `main` halts the machine instead — see below. |

### There is no `setcc`

The ISA can compare and branch, but it has no instruction that writes 0 or 1 into a register from a
condition. When a comparison is used as a *value* (`int b = (x < y);`, or the result of `&&`/`||`),
the back end has to synthesize it with a short jump:

```casm
ifls r4, r5, .cmp0_true
li   r4, 0
jp   .cmp0_end
.cmp0_true:
li   r4, 1
.cmp0_end:
```

Four instructions where a machine with `setcc` would spend one. That is a real property of the
target, which is why it is written down here rather than buried in the emitter.

### Indexing costs one extra instruction

There is no `base + index × scale` addressing mode. The index is scaled first, and the result is
used as the second register of an indexed access — the assembler picks the indexed opcode by itself,
from the shape of the operands, the same way it picks `ADD` over `ADDI`:

```
%off = mul %i, 4                  mul r5, r5, 4
%v   = load.word [%arr + %off]    ldr r6, [r4 + r5]
```

### Division by zero does not fault

The VM sets its Trap flag and leaves the destination register alone; nothing reads that flag
automatically. Ceres-C inherits the behaviour rather than hiding it behind an implicit check.

### `&&` and `||` are control flow

The right-hand side is never evaluated when the left already decides the answer, so they lower to
blocks and branches rather than to a `BinOp`:

```
    br.ne %a, 0, L1, Lfalse
L1: br.ne %b, 0, Ltrue, Lfalse
Ltrue:  %t = const 1;  jmp Lend
Lfalse: %t = const 0
Lend:
```

`switch` is the one place a dispatch may take one of three shapes, chosen by
`IrBuilder::emitSwitchDispatch` (see [13-Switch-Jump-Table-Plan.md](13-Switch-Jump-Table-Plan.md)):

- a **jump table** (`tbl.jmp %x - low, [L…], default Ld`) when the case values are dense enough -
  one `TableJump` whose `.rodata` table holds the case blocks' addresses, dispatched with an
  indexed load and an indirect jump;
- a **balanced tree** of `<`/`==` tests when the values are too sparse for a table but numerous
  enough to beat the chain;
- the plain **comparison chain** otherwise - and always at `-O0` or under `-fno-jump-tables`.

`TableJump` is the one terminator whose successor set is neither one block nor two: the optimizer's
CFG walks (`successorsOf`) list every table entry plus `default`, so nothing reachable only through
the table is collected as dead.

## Registers and frames

The allocation rule is deliberately small enough to state in a paragraph.

Every value has one home. A value may sit in a caller-saved register only while no `call` can
clobber it — a call destroys `r0`–`r7`, `r12`, `f0`–`f7` and the flags. So a temporary gets one
whenever its own live range is call-free, and a local gets one only in a function that calls nothing
at all. The callee-saved half — `r8`–`r11` and `f8`–`f15` — is the exception a call does *not*
clobber, so a local that has to survive a call may live there instead; the price is that the
function saves and restores each one around its own body (`pushm`/`popm`, `fpushm`/`fpopm`). The
saved copies sit below the frame, which is why an incoming stack argument is read at
`[fp + 8 + 4·saved]` rather than `[fp + 8]`. Everything else lives in a field of the function's
stack frame.

Width is the other half of the rule, and it is simply "does it fit": a `char`, a `bool`, a `short`,
an `int`, a `float` and a pointer are all candidates, an array or a `struct` is not. A narrow local
in a register holds exactly what a narrow frame field would have held, because the IR keeps a value
of narrow type in its already-narrowed representation at all times — so the `strb`/`ldrsb` pair a
`char` field would have gone through has nothing left to do. The one value that does not come from
inside the function is a narrow *parameter*, and the prologue narrows it into its register with the
one instruction (`sxtb`, `sxth` or an `and` mask) the store into a field used to perform for free.

At `-O0` there is no allocation at all: every local, parameter and temporary gets its own permanent
frame field. That path stays reachable on purpose — it needs no analysis to be correct, so it is
what you bisect against when an optimized program misbehaves.

### The frame is a CASM `struct`

Ceres-C does not compute byte offsets. It emits a real `struct` per function and lets the assembler
lay it out, exactly as a hand-written program following the calling convention would:

```casm
struct __frame_suma_array
    slot0: u32
endstruct
global suma_array:
    enter __frame_suma_array
    ...
    leave
    ret
```

Symbolic `[sp + __frame_x.slotN]` references then stay correct by construction, instead of this
project's layout arithmetic drifting out of sync with the assembler's.

A function that needs nothing from a frame — few enough arguments, no local that has to live in
memory, nothing held across a call — skips `enter`/`leave` entirely and just returns. "Leaf" is
shorthand: the real condition is "needs nothing from a frame", which a function that makes a call
can still satisfy, because `call`/`ret` put the return address on the hardware stack rather than in
the frame.

### Variadic functions always have a frame

A variadic function reads its argument tail out of the **caller's** frame, at `[fp + 8]` and upward,
so it needs an `fp` of its own to measure from — which means a real `enter`, even when it is a leaf
that would otherwise have qualified for the frameless-leaf rule above.

The tail is passed entirely in the outgoing stack area, never in `r0`–`r3` or `f0`–`f3`, which is
what makes a statically known `[fp + N]` the right answer at all: the callee does not know the types
of its tail, so it could not tell which bank an argument had been put in. `VaStart` produces that
one address, and everything else — `__builtin_va_arg`, `__builtin_va_copy`, `__builtin_va_end` — is
ordinary pointer work on top of it.
[09-Variadic-Convention.md](09-Variadic-Convention.md) is the full contract.

### Names and linkage

**A C symbol keeps its own name.** `int triple(int)` is `triple` in the generated assembly, and a
routine written in CASM as `global triple:` is what `extern int triple(int);` names. That is what
makes interoperability work in both directions without a decoder ring — see
[07-CASM-Interop.md](07-CASM-Interop.md).

The price is a small collision class: a C identifier that happens to be one of CeresASM's reserved
words (`let`, `global`, `struct`, `word`, `u32`, `align`, `true`, a register name, ...) cannot be
read as an identifier by the assembler. Ceres-C reports that as its own diagnostic, naming the
declaration and asking you to rename it, rather than letting it surface as an assembler syntax
error pointing at generated text.

`global` in front of a label or a `let` is what publishes a symbol to the linker
(12-Labels-and-Symbols.md), so it is exactly C's external linkage:

| C | Generated CASM |
| --- | --- |
| `int f(void) { ... }` | `global f:` |
| `static int f(void) { ... }` | `f:` — internal, and droppable if nothing calls it |
| `int counter = 0;` | `global let counter: u32 = 0` in `@data` |
| `static int counter = 0;` | `let counter: u32 = 0` |
| `const int limit = 3;` | `global let limit: u32 = 3` in **`@rodata`** |
| `extern int counter;` | nothing — the storage belongs to whoever defines it |
| `static int n;` inside `f` | `let f__n: u32` in `@bss` — file-scope storage, no linkage |

A `const` global with an initializer goes to `@rodata`, where the machine itself enforces the
qualifier: a store into it raises `MemoryFault` rather than quietly working. That is also why sema
refuses to convert a `const int*` to an `int*` — the promise is not a formality.

### Frames of any size

A load or store displacement is a signed 16-bit field, and `enter Frame` takes the frame size as a 16-bit immediate.
At -O0 every local and temporary has its own slot, so a function with thousands of locals outgrows both. The
generator knows where each slot sits (it lays the frame struct out the way the assembler does), so a slot beyond
32 KiB is reached through the assembler temporary - `la at, <offset>` / `add at, at, sp` / `[at + 0]` - and a frame
beyond 64 KiB starts with a bare `enter` followed by `la at, <size>` / `sub sp, sp, at`. Nothing changes for a
function that fits; `at` (r13) is never allocated to a value.

### How `main` ends

`main` has no caller. Instead of `ret`, the generated code writes the shutdown command to the system
control device and halts. A word write carries the exit status in bits 15:8, so `return n;` is:

```casm
la   r4, 0xFFFF0000
shl  r5, r0, 8         // the status, above the command
or   r5, r5, 1         // 1 = shut down
str  [r4 + 0], r5
halt
```

`ceres run` exits with that status (its low eight bits) and with 1 on a fault. A `return;` with no value
writes the command alone (`li r5, 1` / `str`), which is status 0. Device registers take 32-bit accesses only.

A unit that declares `void exit(int)` ends `main` differently: it calls it, so falling off `main` is
`exit(main())` and the C library's `atexit` handlers and stream flushing run:

```casm
call exit              // r0 holds the status (or `li r0, 0` for a `void main`)
halt
```

## Every line cites its source

Each emitted instruction carries the file and line of C it came from:

```casm
    mul r4, r12, 4        // examples/15_suma_array.c:22
    str [sp + __frame_suma_array.slot0], r4 // examples/15_suma_array.c:22
    ldr r5, [sp + __frame_suma_array.slot0] // examples/15_suma_array.c:22
    ldr r2, [r2 + r5]     // examples/15_suma_array.c:22
```

That is the whole reason this compiler emits text. [04-Tutorial-C-to-CASM.md](04-Tutorial-C-to-CASM.md)
follows one program through all of it.

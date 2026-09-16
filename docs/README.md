# Ceres-C documentation

Ceres-C compiles a subset of C to **CeresASM text** — readable `.casm`, not an object file. The
generated assembly is meant to be opened and read: every instruction carries a comment naming the
line of C it came from, which is the whole reason this compiler emits text instead of bytes.

| Document | What is in it |
| --- | --- |
| [01-Getting-Started.md](01-Getting-Started.md) | Building Ceres-C, wiring up a CeresASM checkout, compiling and running your first program. |
| [02-Grammar.md](02-Grammar.md) | The C subset: what is in, what is out, and the EBNF the parser implements. |
| [03-IR-to-CASM.md](03-IR-to-CASM.md) | The intermediate representation and how each of its instructions becomes CASM. Registers, frames, the calling convention. |
| [04-Tutorial-C-to-CASM.md](04-Tutorial-C-to-CASM.md) | One program followed through every stage of the pipeline, with the compiler's real output at each one. |
| [05-CLI.md](05-CLI.md) | Every `ceresc` option, including the optimization switches. |
| [06-Known-Limitations.md](06-Known-Limitations.md) | What this version leaves out, and why. |
| [07-CASM-Interop.md](07-CASM-Interop.md) | Building one program out of C and hand-written assembly, in both directions. |
| [08-Preprocessor.md](08-Preprocessor.md) | `#include`, `#define`, headers, and what is missing. |
| [09-Variadic-Convention.md](09-Variadic-Convention.md) | `...`, `va_list` and the four builtins: where a variadic argument is passed and how the callee finds it. |
| [10-Interrupts.md](10-Interrupts.md) | `__interrupt` handlers, binding a vector, and the three builtins that mask and wait. |

## The pipeline

```
file.c ──▶ preprocessor ──▶ lexer ──▶ parser ──▶ sema ──▶ ir ──▶ codegen ──▶ file.casm
                                                                                 │
                                                  ceres asm -c  ◀────────────────┘  (separate process)
                                                       │
                                                       ▼
                                                   file.cobj ──▶ ceres link ──▶ file.cres ──▶ ceres run
```

Eight libraries under `libs/`, one per stage, each buildable and testable on its own. Everything
after `file.casm` is deliberately a separate process: Ceres-C never links against CeresASM's
assembler, it invokes the `ceres` binary exactly as you would from a terminal.

Several `.c` files can go through that pipeline at once, and a hand-written `.casm` can join them at
the link — see [07-CASM-Interop.md](07-CASM-Interop.md).

## The shipped examples

Seventeen single-file programs under [`examples/`](../examples), each with a `.expected` file holding
the exact output it must produce, plus [`examples/interop/`](../examples/interop) — one program built
from two C files, a header and two hand-written `.casm` files. Every one of them is compiled,
assembled and run at `-O0`, `-O1` and `-O2` on every build (`tests/examples`), and all three levels
must print the same bytes — that is what makes an optimization bug a build failure rather than a
surprise.

Start with [`01_return_constant.c`](../examples/01_return_constant.c) and read them in order; they
are written to be read as a course in what the subset can do.

# The `ceresc` command line

[← Back to index](README.md)

```
usage: ceresc <file.c|file.casm>... [-o <output>] [-I <dir>] [-D <name>[=<value>]]
               [--emit-ast] [--emit-ir] [-E] [-S | --run] [--ceres-path <dir>]
               [-Werror] [-O<level>] [-f<opt>]
       ceresc --version | --help
```

Several inputs are allowed, and they need not all be C: a `.casm` file is assembled and linked
alongside the compiled ones, which is how a routine written in assembly becomes callable from C
(see [07-CASM-Interop.md](07-CASM-Interop.md)).

## Options

| Option | Effect |
| --- | --- |
| `-o <output>` | With a single C input and no `--run`, the `.casm` to write. Otherwise the linked program. |
| `-I <dir>` | A directory to search for `#include`. Repeatable; searched in order. Also spelled `-Idir`. |
| `-D <name>[=<value>]` | Predefine an object-like macro. A bare name means `1`. Also spelled `-DNAME=1`. |
| `--emit-ast` | Print the type-checked syntax tree and stop. |
| `--emit-ir` | Print the IR — after optimization, so it is what the back end will actually be handed — and stop. |
| `-E` | Print the preprocessed source and stop. |
| `-S` | Stop at the CASM text; do not assemble it. This is the default. |
| `--run` | Assemble every unit, link them, and run the result, forwarding its output and exit code. |
| `--ceres-path <dir>` | Where to find the `ceres` binary. Without it, `ceres` is looked up on `PATH`. |
| `-Werror` | Treat warnings as errors. |
| `--version` | Print the version and stop. Works anywhere on the line and needs no input file. |
| `--help`, `-h` | Print the usage text. |

`-S` and `--run` are opposites and resolve in **argument order**: the last one written wins. That is
what makes `-S` worth having — `ceresc main.c --run -S` stops at the text, so adding `-S` to a
command you already had is enough to stop it running.

## What gets written where

One C input, no `--run`:

```sh
ceresc main.c                 # writes main.casm
ceresc main.c -o out.casm     # writes out.casm
```

Several inputs: each `.c` becomes a `.casm` next to its own source, because one output name cannot
stand for several files, and `-o` names the final program instead:

```sh
ceresc io.c main.c -o hello.cres --run
#   io.casm, main.casm        one per C source
#   hello.decls.casm          the declarations every generated unit imports
#   io.cobj, main.cobj        one object per unit
#   hello.cres                the linked program
```

The declarations file only appears when there is more than one thing to link — a lone translation
unit resolves every name it uses by itself.

## Assembling and running

`--run` is four subprocesses, all of them the real `ceres` binary:

```sh
ceres asm -c io.casm   -o io.cobj
ceres asm -c main.casm -o main.cobj
ceres link io.cobj main.cobj -o hello.cres
ceres run  hello.cres
```

Ceres-C never links against CeresASM's own libraries — it runs the same commands you would, which is
what makes them the ones to reach for when something goes wrong.

## Optimization

Optimizations are **on by default**: no `-O` flag means `-O1`.

| Level | What it turns on |
| --- | --- |
| `-O0` | Nothing. Every value gets a permanent frame field, every function gets a frame, and no rewrite runs. |
| `-O1` | Everything except inlining. The default. |
| `-O2` | Everything, inlining included. |
| `-O3` | Accepted as an alias for `-O2`. There is no third tier; failing a build over a habit every other C compiler tolerates helps nobody. |

Each individual optimization also has its own switch, and a `-f`/`-fno-` **overrides whatever `-O`
set, in argument order** — the same resolution rule as `-S`/`--run`:

```sh
ceresc main.c -O2 -fno-inline          # everything except inlining
ceresc main.c -O0 -fconst-fold         # only constant folding
```

`ceresc --help` lists every optimization by name with a one-line description, generated from the
same table the parser validates against, so the two cannot drift apart.

### Why `-O0` is kept working

Every optimization has a simplified counterpart that is still reachable and still tested. A program
that behaves differently at two levels is a miscompilation, and `-O0` is the fixed point you bisect
against: turn individual `-f` switches off until the two agree again, and the last one you turned
off is the one at fault. `tests/e2e` and `tests/examples` both run every program at all three levels
and require identical output for exactly this reason.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | Success, or a question answered (`--help`, `--version`, no arguments). |
| 1 | A malformed command line, an unreadable input, or any phase that reported an error. |
| other | Passed straight through from `ceres asm`, `ceres link` or `ceres run` under `--run`. |

Diagnostics go to stderr in the usual `file:line:column: severity: message` form, and the compiler
does not stop at the first one — the parser resynchronizes and sema keeps checking, so one run
reports everything it can see. A diagnostic about a line that came from a header names the header
(see [08-Preprocessor.md](08-Preprocessor.md#line-numbers-across-an-include)).

## Environment

| Variable | Used by | Meaning |
| --- | --- | --- |
| `CERESC_CERES_PATH` | the test suites, not the compiler | Directory holding `ceres`/`ceres.exe`. Without it the `e2e` and `examples` suites skip instead of failing. |

The compiler itself takes that path from `--ceres-path` or `PATH`; the variable exists so CI can
point the suites at a CeresASM checkout it built in the same job.

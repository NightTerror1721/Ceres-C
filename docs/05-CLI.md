# The `ceresc` command line

[← Back to index](README.md)

```
usage: ceresc <file.c> [-o <file.casm>] [--emit-ast] [--emit-ir] [-S | --run]
               [--ceres-path <dir>] [-Werror] [-O<level>] [-f<opt>]
       ceresc --version | --help
```

One input file per invocation. There is no separate-compilation story yet: a translation unit is a
whole program.

## Options

| Option | Effect |
| --- | --- |
| `-o <file.casm>` | Where to write the generated CASM. Defaults to the input's own path with a `.casm` extension. |
| `--emit-ast` | Print the type-checked syntax tree and stop. |
| `--emit-ir` | Print the IR — after optimization, so it is what the back end will actually be handed — and stop. |
| `-S` | Stop at the CASM text; do not assemble it. This is the default. |
| `--run` | Also invoke `ceres asm` and then `ceres run` on the result, forwarding their output and exit code. |
| `--ceres-path <dir>` | Where to find the `ceres` binary. Without it, `ceres` is looked up on `PATH`. |
| `-Werror` | Treat warnings as errors. |
| `--version` | Print the version and stop. Works anywhere on the line and needs no input file. |
| `--help`, `-h` | Print the usage text. |

`-S` and `--run` are opposites and resolve in **argument order**: the last one written wins. That is
what makes `-S` worth having — `ceresc main.c --run -S` stops at the text, so adding `-S` to a
command you already had is enough to stop it running.

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
| other | Passed straight through from `ceres asm` or `ceres run` under `--run`. |

Diagnostics go to stderr in the usual `file:line:column: severity: message` form, and the compiler
does not stop at the first one — the parser resynchronizes and sema keeps checking, so one run
reports everything it can see.

## Environment

| Variable | Used by | Meaning |
| --- | --- | --- |
| `CERESC_CERES_PATH` | the test suites, not the compiler | Directory holding `ceres`/`ceres.exe`. Without it the `e2e` and `examples` suites skip instead of failing. |

The compiler itself takes that path from `--ceres-path` or `PATH`; the variable exists so CI can
point the suites at a CeresASM checkout it built in the same job.

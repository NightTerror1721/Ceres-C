# Ceres-C

A compiler for a subset of C that emits **readable CeresASM text**, written in C++23.

```sh
ceresc examples/07_structs.c --run
```

It is a sibling project to [CeresASM](https://github.com/Krampus1721/CeresASM), not a part of it:
Ceres-C never links against the assembler or the VM. It writes `.casm` and, when asked, invokes the
`ceres` binary as a subprocess — exactly as you would from a terminal.

The output is the point. Every generated instruction carries a comment naming the line of C it came
from, so the assembly can be read alongside the source instead of taken on trust:

```casm
global suma_array:
.L2:
    mov r3, r6            // examples/15_suma_array.c:22
    mov r2, r0            // examples/15_suma_array.c:22
    mov r12, r7           // examples/15_suma_array.c:22
    mul r4, r12, 4        // examples/15_suma_array.c:22
    ...
    add r12, r3, r2       // examples/15_suma_array.c:22
```

A C symbol keeps its own name there, which is what lets a routine written by hand in CeresASM be
called from C and vice versa — see [docs/07-CASM-Interop.md](docs/07-CASM-Interop.md).

## Building

CMake 3.28+, Ninja, and a C++23 compiler. No other dependencies — no package manager, no
third-party libraries, no generated sources.

```sh
cmake --preset gcc
cmake --build --preset gcc-debug
ctest --preset gcc-debug
```

`msvc`, `clang` and `ninja` presets exist too, each building Debug and Release from one tree. The
binary lands in `build/<preset>/bin/<config>/`. Visual Studio users open the *folder*; it reads
`CMakePresets.json` on its own.

To assemble and run what the compiler produces you also need a built CeresASM checkout. See
[docs/01-Getting-Started.md](docs/01-Getting-Started.md).

## Using it

```sh
ceresc program.c -o program.casm      # compile to CASM text (the default)
ceresc program.c --run                # ...and assemble and run it with `ceres`
ceresc io.c main.c lib.casm -o app.cres --run   # several files, C and assembly, linked together
ceresc program.c -I include -D DEBUG  # include search path, predefined macro
ceresc program.c -E                   # show the preprocessed source
ceresc program.c --emit-ir            # show the intermediate representation
ceresc program.c --emit-ast           # show the type-checked syntax tree
ceresc program.c -O0                  # no optimizations: the verbose, obvious output
```

Full option list in [docs/05-CLI.md](docs/05-CLI.md).

### Hello, terminal

Ceres-C has no standard library: no `printf`, no `malloc`, nothing to include. A program prints the
same way a hand-written CASM program does — by storing a byte into the terminal device's output
register.

```c
int main(void)
{
    char* terminal = (char*)0xFF000004;
    *terminal = 'h';
    *terminal = 'i';
    *terminal = '\n';
    return 0;
}
```

## The language

`void`, `bool`, `char`, `short`, `int`, `long`, `float`, with `signed`/`unsigned` and
`short`/`long` combining as they do in C. `const`, `static`, `extern`, `auto` and `inline`.
Pointers, fixed-size arrays, `struct`, `enum`, `typedef`. Every statement form including
`do`/`while`, `switch` and `goto`. Every operator including short-circuit `&&`/`||`, compound
assignment, `++`/`--` in both positions, `sizeof`, casts, `?:`, and `.` and `->` as genuinely
distinct operators.

Headers work: `#include`, `#define` for object-like, function-like and variadic macros, `#undef`,
`#pragma once`, conditional compilation (`#if`/`#elif`/`#else`/`#ifdef`/`#ifndef`/`defined`) and
`#error`/`#warning` — see [docs/08-Preprocessor.md](docs/08-Preprocessor.md).

`union` and `alignof` are supported, and so are the `volatile`, `restrict` and `register`
qualifiers. Variadic functions work too, with `va_list` and the four builtins — see
[docs/09-Variadic-Convention.md](docs/09-Variadic-Convention.md).

No `double` (the VM has no f64 at all), no bitfields, no function pointers.
[docs/02-Grammar.md](docs/02-Grammar.md) is the contract;
[docs/06-Known-Limitations.md](docs/06-Known-Limitations.md) is the honest list of what this version
leaves out.

## Documentation

| | |
| --- | --- |
| [Getting started](docs/01-Getting-Started.md) | Build it, wire up CeresASM, run your first program. |
| [The C subset](docs/02-Grammar.md) | What is in, what is out, and the EBNF. |
| [From IR to CASM](docs/03-IR-to-CASM.md) | The IR, the instruction mapping, registers and frames. |
| [Tutorial: C to CASM](docs/04-Tutorial-C-to-CASM.md) | One program through every stage, with real output. |
| [Command line](docs/05-CLI.md) | Every option, including the optimization switches. |
| [Known limitations](docs/06-Known-Limitations.md) | What this version leaves out, and why. |
| [C and CASM together](docs/07-CASM-Interop.md) | One program out of C and hand-written assembly. |
| [The preprocessor](docs/08-Preprocessor.md) | `#include`, `#define`, headers, conditionals. |
| [Variadic functions](docs/09-Variadic-Convention.md) | `...`, `va_list`, and where a variadic argument is passed. |

## Examples

Seventeen programs in [`examples/`](examples), meant to be read in order — each one introduces one
part of the language and prints something you can check.

| | | | |
| --- | --- | --- | --- |
| `01_return_constant` | `05_arrays` | `09_switch_goto` | `13_sorting` |
| `02_arithmetic` | `06_pointers` | `10_types` | `14_bitwise` |
| `03_control_flow` | `07_structs` | `11_floats` | `15_suma_array` |
| `04_functions` | `08_strings` | `12_globals_and_enums` | `16_typedef_stack` |
| | | | `17_variadic` |

Plus [`examples/interop/`](examples/interop): one program built from two C files, a header and two
hand-written `.casm` files, which is the whole multi-file and assembly-interop story in one place.

Each has a sibling `.expected` holding the exact bytes it must print. Every one is compiled,
assembled and run at `-O0`, `-O1` **and** `-O2` on every build, and all three levels must agree —
that is what turns an optimization bug into a build failure instead of a surprise.

## How it is put together

```
file.c ─▶ preprocessor ─▶ lexer ─▶ parser ─▶ sema ─▶ ir ─▶ codegen ─▶ file.casm
                                                                          │
                                        ceres asm -c ─▶ ceres link ─▶ ceres run
```

Eight libraries under `libs/`, one per stage, plus `libs/support` underneath all of them and
`libs/driver` on top. No library knows about the one that consumes it: `libs/parser` does not know
`libs/sema` exists, and `libs/ir` does not know `libs/codegen` does. That is what makes each stage
testable without building the whole compiler.

```
libs/support       SourceManager, DiagnosticEngine, Arena
libs/preprocessor  #include/#define, and the line map that keeps diagnostics honest
libs/lexer         characters -> tokens
libs/ast           the tree and the type system (data only)
libs/parser        tokens -> tree
libs/sema          tree -> annotated tree (types, symbols, layout)
libs/ir            annotated tree -> three-address IR, and the optimizer
libs/codegen       IR -> CASM text
libs/driver        the pipeline, the command line, the subprocesses
apps/ceresc        the binary
```

## Tests

```sh
ctest --preset gcc-debug
```

Eleven suites: one per library, plus `e2e` (compile, assemble and run real programs through the
real `ceres`) and `examples` (the directory above, checked against its `.expected` files). The
framework is ninety lines in `tests/framework/` — this project has no package manager, and Catch2
would cost more in build plumbing than that.

`e2e` and `examples` need a `ceres` binary and **skip** without one, so a checkout of Ceres-C alone
still goes green. Point them at a CeresASM checkout to run them for real:

```sh
CERESC_CERES_PATH=/path/to/CeresASM ctest --preset gcc-debug
```

## License

See [LICENSE](LICENSE).

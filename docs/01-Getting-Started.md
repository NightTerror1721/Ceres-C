# Getting started

[← Back to index](README.md)

## What you need

- A C++23 compiler. MSVC 19.4x, GCC 15 and Clang 18 are the three this is built with.
- CMake 3.28 or newer, and Ninja.
- A built [CeresASM](https://github.com/Krampus1721/CeresASM) checkout, if you want to assemble and
  run what Ceres-C produces. Compiling to `.casm` text needs nothing but Ceres-C itself.

There are no other dependencies. Ceres-C has no package manager, no third-party libraries and no
generated sources — the test framework is ninety lines in `tests/framework/`.

## Building

Everything goes through the CMake presets, so what CI builds is what you build:

```sh
cmake --preset gcc
cmake --build --preset gcc-debug
ctest --preset gcc-debug
```

`--preset msvc` and `--preset clang` are the same on their toolchains, and `--preset ninja` uses
whatever compiler the environment provides. Each preset configures Debug and Release from one tree;
the binary lands in `build/<preset>/bin/<config>/`.

Visual Studio users: open the folder, not a solution. VS reads `CMakePresets.json` on its own.

### Presets

| Configure preset | Build presets | Notes |
| --- | --- | --- |
| `msvc` | `msvc-debug`, `msvc-release` | Ninja Multi-Config with `cl`. Needs a developer command prompt. |
| `gcc` | `gcc-debug`, `gcc-release` | |
| `clang` | `clang-debug`, `clang-release` | |
| `ninja` | `ninja-debug`, `ninja-release` | Whatever compiler is on `PATH`. |
| `msvc-ipo`, `gcc-ipo`, `clang-ipo` | `<name>-release` | Whole-program optimization. Slower to build; for the binary you ship. |

## Pointing Ceres-C at CeresASM

`ceresc --run` assembles and runs what it just compiled by launching the `ceres` binary as a
subprocess. Set `CERES_PATH` to the directory that holds it (or to the executable) once, and every build finds it;
otherwise it is looked for on `PATH`, and `--ceres-path` overrides both for one command:

```sh
export CERES_PATH=/path/to/CeresASM
ceresc examples/07_structs.c --run
ceresc examples/07_structs.c --run --ceres-path ../CeresASM
```

The order and the errors are in [05-CLI.md](05-CLI.md#finding-ceres).

The test suites need the same binary, and take it from the `CERESC_CERES_PATH` environment variable
(a directory containing `ceres`/`ceres.exe`) or from a CeresASM checkout sitting next to this one:

```sh
export CERESC_CERES_PATH=/path/to/CeresASM
ctest --preset gcc-debug
```

Without it, the `e2e` and `examples` suites **skip** rather than fail, so a checkout of Ceres-C on
its own still goes green. They also find a CeresASM checkout sitting next to this one on their own,
by walking up from wherever the test runs. A CI job that means to test the whole path is responsible
for setting the variable — [`.github/workflows/ci.yml`](../.github/workflows/ci.yml) does.

## Your first program

Ceres-C has no standard library: no `printf`, no `malloc`, and no header to include for them. A
program prints by storing a byte into the terminal device's output register, exactly as a
hand-written CASM program does:

```c
int main(void)
{
    volatile unsigned int* terminal = (volatile unsigned int*)0xFF000004;
    *terminal = 'h';
    *terminal = 'i';
    *terminal = '\n';
    return 0;
}
```

```sh
ceresc hi.c --run
hi
```

`main` does not return to anyone. The generated code halts the machine through the system control
device instead, so `return 0` means "stop" and nothing else — `ceres run` exits 0 on a clean halt
and 1 on a fault, never with a value the program chose. Anything a program wants to report, it
prints.

## More than one file

Name every piece on one command line. A `.c` is compiled; a `.casm` is assembled and linked
alongside it:

```sh
ceresc io.c hello.c triple.casm -o hello.cres --run
```

Headers work the way you expect — `#include`, `#define` for object-like macros, and `#pragma once`
as the include guard (there is no `#ifndef` in this version). `-I` adds a search directory and `-D`
predefines a macro:

```sh
ceresc src/main.c -I include -D DEBUG --run
```

[`examples/interop/`](../examples/interop) is a complete program of this shape: two C files, a
header, and two hand-written assembly files that call into C and are called from it. See
[07-CASM-Interop.md](07-CASM-Interop.md) and [08-Preprocessor.md](08-Preprocessor.md).

## Looking at what it produced

The point of emitting text is that you can read it:

```sh
ceresc examples/15_suma_array.c -o suma.casm    # the CASM, with a comment per line of C
ceresc examples/15_suma_array.c --emit-ir       # the intermediate representation
ceresc examples/15_suma_array.c --emit-ast      # the type-checked syntax tree
ceresc examples/15_suma_array.c -E              # the preprocessed source
```

[04-Tutorial-C-to-CASM.md](04-Tutorial-C-to-CASM.md) walks that exact file through every one of them.

## Running the examples

```sh
ceresc examples/13_sorting.c --run
```

Each `examples/*.c` has a sibling `.expected` holding the exact bytes it must print. `ctest` checks
every one of them, at all three optimization levels, on every build.

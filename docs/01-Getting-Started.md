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
subprocess. It finds it on `PATH`, or wherever `--ceres-path` says:

```sh
ceresc examples/07_structs.c --run
ceresc examples/07_structs.c --run --ceres-path ../CeresASM
```

The test suites need the same binary, and take it from the `CERESC_CERES_PATH` environment variable
(a directory containing `ceres`/`ceres.exe`) or from a CeresASM checkout sitting next to this one:

```sh
export CERESC_CERES_PATH=/path/to/CeresASM
ctest --preset gcc-debug
```

Without it, the `e2e` and `examples` suites **skip** rather than fail, so a checkout of Ceres-C on
its own still goes green. A CI job that means to test the whole path is responsible for setting the
variable — [`.github/workflows/ci.yml`](../.github/workflows/ci.yml) does.

## Your first program

Ceres-C has no standard library and no preprocessor. There is no `#include`, no `printf` and no
`malloc`. A program prints by storing a byte into the terminal device's output register, exactly as
a hand-written CASM program does:

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

```sh
ceresc hi.c --run
hi
```

`main` does not return to anyone. The generated code halts the machine through the system control
device instead, so `return 0` means "stop" and nothing else — `ceres run` exits 0 on a clean halt
and 1 on a fault, never with a value the program chose. Anything a program wants to report, it
prints.

## Looking at what it produced

The point of emitting text is that you can read it:

```sh
ceresc examples/15_suma_array.c -o suma.casm    # the CASM, with a comment per line of C
ceresc examples/15_suma_array.c --emit-ir       # the intermediate representation
ceresc examples/15_suma_array.c --emit-ast      # the type-checked syntax tree
```

[04-Tutorial-C-to-CASM.md](04-Tutorial-C-to-CASM.md) walks that exact file through all three.

## Running the examples

```sh
ceresc examples/13_sorting.c --run
```

Each `examples/*.c` has a sibling `.expected` holding the exact bytes it must print. `ctest` checks
every one of them, at all three optimization levels, on every build.

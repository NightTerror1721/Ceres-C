# The preprocessor

[← Back to index](README.md)

A text-to-text pass that runs before the lexer, turning a source file plus everything it includes
into one buffer.

| Directive | What it does |
| --- | --- |
| `#include "file.h"` | Next to the including file first, then the search path. |
| `#include <file.h>` | The search path only (`-I`). |
| `#define NAME text` | An object-like macro: every later `NAME` identifier becomes `text`. |
| `#define F(a, b) text` | A function-like macro. Arguments are substituted as whole identifiers. |
| `#define F(a, ...) text` | A variadic macro; `__VA_ARGS__` is the comma-separated remaining arguments. |
| `#` and `##` in a macro body | `#a` stringifies an argument; `a ## b` pastes two tokens into one. |
| `#undef NAME` | Forgets one. |
| `#if`, `#elif`, `#else`, `#endif` | Selects source based on an integer constant expression. |
| `#ifdef`, `#ifndef` | Selects source based on whether a macro is defined. |
| `__has_include("f")`, `__has_include(<f>)` | Inside a `#if`, `1` when the target can be included (resolved exactly as `#include` would), else `0`. |
| `#error message` | Emits an error and stops a successful compilation. |
| `#warning message` | Emits a warning without failing preprocessing. |
| `#pragma once` | This file contributes nothing if it is included again. |
| `#pragma warning(...)` | Turns one of this compiler's own warnings off, back on, or into an error. See [11-Diagnostics.md](11-Diagnostics.md). |
| `\` at the end of a line | Splices the next physical line onto this one, so any line — a `#define` body, an `#if`, ordinary code — may span lines. |

`#if` expressions support integer literals, parentheses, unary `+ - ! ~`, arithmetic, shifts,
comparisons, equality, bitwise operators and `&&`/`||`, with C precedence. Undefined identifiers
evaluate to zero. `defined NAME` and `defined(NAME)` test macro existence without expanding `NAME`.

Anything else — notably `#line` — is reported by name:

```
main.c:1:1: error: '#ifdef' is not supported in this version - only #include, #define, #undef and
                   #pragma once are
```

That is the point: it is a known gap, not an unreadable line.

## Headers

```c
// io.h
#pragma once

#define TERMINAL_OUT 0xFF000004

void put(char c);
extern int writeCount;
```

```c
// io.c
#include "io.h"

int writeCount = 0;

void put(char c) { char* out = (char*)TERMINAL_OUT; *out = c; writeCount++; }
```

```c
// main.c
#include "io.h"

int main(void) { put('h'); return 0; }
```

Three things are doing work there:

- **`#pragma once`** is the only include guard available. Without `#ifndef` there is no way to write
  the classic three-line one, so a header included twice — directly, or through another header —
  would declare everything twice. Put it at the top of every header.
- **`extern int writeCount;`** declares without defining. `io.c` defines the object; every unit that
  includes the header just learns the name and the type.
- **A prototype** does the same for a function, which needs no keyword: a function has external
  linkage by default.

A name may be declared several times at file scope as long as the declarations agree and at most one
has an initializer — which is exactly what a header's `extern` plus a source file's definition is.

## Macros

Both object-like and function-like macros are supported:

```c
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define TRACE(...) print(__VA_ARGS__)
```

Substitution is by whole identifier, and never inside a string literal, a character literal or a
comment:

```c
#define NAME ceres
char* s = "NAME";      // stays "NAME"
int NAME;              // becomes int ceres;
int NAMEx;             // stays NAMEx - a different identifier
```

A macro may expand into another one. A macro defined in terms of itself stops being rewritten after
a bounded number of passes, with a warning, instead of looping forever. This is text substitution:
stringification and token pasting work on text rather than on tokens — see below.

A backslash immediately before the newline splices the next physical line onto the current one, so a
macro body can be laid out over several lines:

```c
#define SUM(a, b) \
    ((a) + (b))
```

The splice happens before a line is recognised as a directive or expanded, so it applies to `#define`
bodies, `#if` expressions, and ordinary code alike.

### Stringification and token pasting

`#a` inside a function-like macro's body turns an argument into a string literal, and `a ## b`
pastes two tokens into one:

```c
#define NAME(n) #n
#define CAT_(a, b) a ## b
#define CAT(a, b) CAT_(a, b)
#define UNIQUE(prefix) CAT(prefix, __COUNTER__)

const char* label = NAME(writeCount);   // "writeCount"
int UNIQUE(buf);                        // buf0
int UNIQUE(buf);                        // buf1
```

Stringification drops leading and trailing whitespace, collapses a run of internal whitespace into
one space, and escapes `\` and `"` — so the result is always a valid literal. `##` is text pasting:
it deletes the operator and any whitespace around it, leaving the two neighbours as one, with no idea
of what a valid token is. An argument next to `##` is pasted before it is expanded, so pasting a
macro's *result* — like `__COUNTER__` above — needs the one-level indirection `CAT` gives it, exactly
as in C. That is what gives `__COUNTER__` its full weight: a fresh name per expansion, which nothing
else could build.

`-D` predefines one from the command line, and a bare name means `1`:

```sh
ceresc main.c -D DEBUG -D WIDTH=320
```

## Predefined macros

These are defined before the first line of the file is read, so nothing declares them and nothing
includes them:

| Macro | Expands to |
| --- | --- |
| `__LINE__` | The line it is written on, in the file it is written in — not in the expanded text. |
| `__FILE__` | That file's name, as a string literal. A header gets its own name, not the includer's. |
| `__DATE__` | The date this compilation started, `"Mmm dd yyyy"`, day space-padded. |
| `__TIME__` | The time this compilation started, `"hh:mm:ss"`. |
| `__STDC__` | `1`. |
| `__STDC_HOSTED__` | `0`. |
| `__CERESC__` | `1` — this compiler, as opposed to any other. |
| `__BASE_FILE__` | The `.c` the translation unit started from, as a string literal. |
| `__INCLUDE_LEVEL__` | `0` in that `.c`, `1` in a header it includes, `2` one deeper, and so on. |
| `__COUNTER__` | `0`, then `1`, then `2` — a number nobody else has had, per expansion. |

Every one of them is an ordinary entry in the macro table apart from how its replacement is
produced, so `#ifdef __FILE__` is true, `defined(__COUNTER__)` is `1`, and `#undef __LINE__` takes
it away — which, as in C, is then your own problem.

`__DATE__` and `__TIME__` read the clock once per run, not once per use, because C requires every
expansion of either within one translation unit to agree. Both expand to a string literal, and
adjacent string literals are joined into one, so the usual spelling works:

```c
void report(void)
{
    const char* stamp = __DATE__ " " __TIME__;   /* "Sep 17 2026 17:22:22" */
    putstr(stamp);
}
```

`__STDC_HOSTED__` is `0` and means it: there is no `<stdio.h>`, no `<stdlib.h>`, nothing to be
hosted by. `__STDC_VERSION__` is deliberately **not** defined — C89 does not define it either, and
naming a later revision would claim conformance to one this compiler does not implement.
[02-Grammar.md](02-Grammar.md) is the contract instead.

A `-D` on the command line goes in on top of these, so `-D __STDC_HOSTED__=1` is a decision a
program gets to make rather than an error.

## Seeing what it produced

```sh
ceresc main.c -E
```

prints the expanded text and stops — the same buffer the lexer is about to read.

Directives leave a blank line behind rather than disappearing, so a file with no includes keeps one
output line per source line. That is not cosmetic: it means a diagnostic's line number is already
right in the common case, before anything maps it.

## Line numbers across an include

A diagnostic points at the file the line was really written in, not at the expanded text:

```
io.h:7:6: error: variable 'put' declared with type 'void'
```

The expansion records, for every line it writes, which file and which line of it that came from, and
the driver maps a location back through that table before printing. The alternative — emitting
`#line` markers and teaching the lexer to read them — would give the lexer a feature that is not
about lexing.

The same table is what the trailing comments in the generated `.casm` go through, so an instruction
that came out of a header cites the header:

```casm
global helper:
    mov r3, r0            // lib.h:5
    imul r1, r3, 2        // lib.h:5
```

That is why the map itself lives in `libs/support` rather than in this library: `libs/codegen` needs
one and must not know the preprocessor exists.

The column is carried across unchanged. A macro that changed the length of the text earlier on the
same line can move it, which is the one inaccuracy the mechanism accepts in exchange for being this
simple.

## Errors it reports

| Situation | Message |
| --- | --- |
| A header that cannot be found | `cannot find include file 'nowhere.h'` |
| A header that includes itself | `include cycle: '...' includes itself` |
| `#include` with no quotes or angle brackets | `#include expects "file.h" or <file.h>` |
| A macro defined in terms of itself | a warning, after a bounded number of passes |
| A malformed conditional nest | a diagnostic naming the unmatched directive |
| `#error message` | an error containing `message` |
| Any other directive | `'#xxx' is not supported` |

## What is missing, and why

**`__STDC_VERSION__`, `__func__` and `__TIMESTAMP__`.** The first would claim a conformance level
this subset does not have. `__func__` is not a macro at all — it is a predefined *identifier*, which
means a per-function object the compiler synthesizes, not anything the preprocessor could produce.
`__TIMESTAMP__` needs the file's modification time, which nothing here has asked for.

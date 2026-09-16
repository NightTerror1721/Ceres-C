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
| `#undef NAME` | Forgets one. |
| `#if`, `#elif`, `#else`, `#endif` | Selects source based on an integer constant expression. |
| `#ifdef`, `#ifndef` | Selects source based on whether a macro is defined. |
| `#error message` | Emits an error and stops a successful compilation. |
| `#warning message` | Emits a warning without failing preprocessing. |
| `#pragma once` | This file contributes nothing if it is included again. |

`#if` expressions support integer literals, parentheses, unary `+ - ! ~`, arithmetic, shifts,
comparisons, equality, bitwise operators and `&&`/`||`, with C precedence. Undefined identifiers
evaluate to zero. `defined NAME` and `defined(NAME)` test macro existence without expanding `NAME`.

Anything else — notably `#line`, stringification (`#`) and token pasting (`##`) — is reported by name:

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
arguments are not stringified or pasted, and an argument must fit on the same physical source line.

`-D` predefines one from the command line, and a bare name means `1`:

```sh
ceresc main.c -D DEBUG -D WIDTH=320
```

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

**`__FILE__`, `__LINE__` and friends.** They would be easy to add on top of the line map, and
nothing needs them yet.

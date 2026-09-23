# Diagnostics

[← Back to index](README.md)

Every error and warning this compiler can emit has a code, and every message prints it:

```
main.c:12:5: error[E3023]: use of undeclared identifier 'total'
main.c:3:1: warning[W2001]: 'double' is 32 bits here: this machine has no 64-bit floating-point type, so it is exactly 'float'
```

The letter is the severity. The four digits are a number that never changes and never gets reused
for something else, and whose first digit says which stage of the pipeline found the problem:

| Range | Phase | What it is looking at |
| --- | --- | --- |
| `0xxx` | lexer | characters → tokens |
| `1xxx` | preprocessor | `#include`/`#define` and the other directives |
| `2xxx` | parser | tokens → tree |
| `3xxx` | sema | types, symbols, layout |
| `4xxx` | codegen | IR → CASM text |
| `5xxx` | ir | tree → IR |

Errors and warnings are numbered independently inside a phase, so `E3001` and `W3001` both exist
and mean different things. A code classifies the **situation**, not the sentence: the five places
sema says *used type 'x' where arithmetic or pointer type is required* are all `E3048`, because
they are one rule reported from five syntactic positions.

The list [`libs/support/include/ceresc/support/diagnostic_id.h`](../libs/support/include/ceresc/support/diagnostic_id.h)
is the compiler's own copy of the table below, and every `error()`/`warning()` call names an entry
in it.

## Turning a warning off

```c
#pragma warning(disable: 2001)
double total = 0;             /* no W2001 here */
#pragma warning(default: 2001)
```

| Form | What it does |
| --- | --- |
| `#pragma warning(disable: N)` | `WN` is not reported at all from here on. |
| `#pragma warning(default: N)` | Back to what the command line says — a warning, or an error under `-Werror`. |
| `#pragma warning(enable: N)` | A warning from here on, **even under `-Werror`**. |
| `#pragma warning(error: N)` | An error from here on, whatever the command line says. |
| `#pragma warning(push)` | Remembers everything in force. |
| `#pragma warning(pop)` | Puts back exactly what the matching `push` saw. |

`N` is a warning's number, with or without its `W`: `2001` and `W2001` are the same thing. Several
at once are separated by spaces, commas or both, and `all` means every warning there is:

```c
#pragma warning(disable: 2001 3001)
#pragma warning(disable: W2001, W3001)     /* the same */
#pragma warning(error: all)                /* -Werror, from here down */
```

A `push`/`pop` pair is what a header uses to keep a decision to itself — nothing scopes a pragma to
the file it was written in, exactly as in C:

```c
/* noisy.h */
#pragma warning(push)
#pragma warning(disable: 2001)
double counter;
#pragma warning(pop)
```

### What it cannot do

**Only warnings.** `#pragma warning(disable: 3023)` names `E3023`, which is an error, and is
refused with a `W1006` saying so. A program that does not compile does not compile.

**It never fails the compilation.** A pragma is advice. A verb this compiler does not know, a
missing `:`, a `pop` with no `push` — each is a `W1005` naming the part it could not read, and the
program still compiles. That is the same rule `#pragma once` has always followed for an unknown
pragma.

**It is positional, not scoped.** A change applies from the line the pragma is on to the end of the
translation unit, or until something changes it back. There is no block scope and no function
scope; `push`/`pop` is the whole of the scoping story.

## `-Werror`

`-Werror` makes every warning count as a failure. A pragma outranks it in both directions:
`enable:` keeps a warning a warning under `-Werror`, and `error:` makes one an error without it. A
diagnostic promoted by a pragma prints `error[W2001]` — "error" because that is what it now is,
`W2001` because that is still which warning it was.

## The warnings

These are the codes a `#pragma warning(...)` can name.

| Code | Means |
| --- | --- |
| `W1001` | A macro expansion grew past the byte limit and was abandoned. |
| `W1002` | A macro expansion was still changing after the pass limit — usually one defined in terms of itself. |
| `W1003` | Whatever a `#warning` directive was given, verbatim. |
| `W1004` | A `#pragma` this compiler does not know, ignored. |
| `W1005` | A `#pragma warning(...)` this compiler could not read, ignored. |
| `W1006` | A number in a `#pragma warning(...)` that does not name a warning. |
| `W2001` | `double`/`long double` capped to `float` — see [06-Known-Limitations.md](06-Known-Limitations.md). |
| `W2002` | An `__attribute__` that is ignored: one this compiler does nothing with, or `aligned` above 4 — see [02-Grammar.md](02-Grammar.md). |
| `W3001` | A `const` variable with no initializer, which can therefore only ever be zero. |
| `W3002` | A function declared `noreturn` that contains a `return`. |
| `W3003` | A call to a function declared `__attribute__((deprecated))`. |
| `W3004` | The result of a call to a `__attribute__((warn_unused_result))` function is discarded. |

## The errors

### `0xxx` — lexer

| Code | Means |
| --- | --- |
| `E0001` | A `/*` with no `*/`. |
| `E0002` | An integer literal too large to represent. |
| `E0003` | A floating-point literal out of range. |
| `E0004` | `0x` with no digits after it. |
| `E0005` | `0b` with no digits after it. |
| `E0006` | A `\` at the end of a literal. |
| `E0007` | `\x` with no hex digits after it. |
| `E0008` | An escape sequence this compiler does not know. |
| `E0009` | `''`. |
| `E0010` | A character literal with no closing `'`. |
| `E0011` | A character literal holding more than one character. |
| `E0012` | A string literal with no closing `"`. |
| `E0013` | The string pool ran out of memory. |
| `E0014` | A character that cannot start any token. |

### `1xxx` — preprocessor

| Code | Means |
| --- | --- |
| `E1001` | A `#if` expression that does not parse. |
| `E1002` | A function-like macro invocation with no closing `)`. |
| `E1003` | A macro invoked with the wrong number of arguments. |
| `E1004` | A file that includes itself, directly or through another. |
| `E1005` | `#include` nested deeper than the limit. |
| `E1006` | A file that cannot be opened. |
| `E1007` | `#ifdef`/`#ifndef` with no macro name. |
| `E1008` | `#elif` with no matching `#if`. |
| `E1009` | `#else` with no matching `#if`. |
| `E1010` | `#endif` with no matching `#if`. |
| `E1011` | `#include` with neither quotes nor angle brackets. |
| `E1012` | A header that cannot be found on any search path. |
| `E1013` | `#define` with no name. |
| `E1014` | A macro parameter list that does not parse. |
| `E1015` | A macro parameter list with no closing `)`. |
| `E1016` | `#undef` with no name. |
| `E1017` | Whatever a `#error` directive was given, verbatim. |
| `E1018` | A directive this compiler does not implement. |
| `E1019` | A `#if` left open at the end of the file. |

### `2xxx` — parser

| Code | Means |
| --- | --- |
| `E2001` | A machine builtin given arguments; it takes none. |
| `E2002` | The token the grammar needed was not the one that was there. |
| `E2003` | The arena ran out of memory. |
| `E2004` | `.` or `->` with no member name after it. |
| `E2005` | An expression was needed and something else was there. |
| `E2006` | The same qualifier written twice. |
| `E2007` | Two storage-class specifiers on one declaration. |
| `E2008` | A storage class other than `register` on a parameter. |
| `E2009` | A storage class in a type name, where there is no declaration to own it. |
| `E2010` | A name used where a type was needed. |
| `E2011` | `restrict` on something that is not a pointer. |
| `E2012` | A type name was needed and something else was there. |
| `E2013` | `struct`/`union` with no tag name. |
| `E2014` | A `struct`/`union`/`enum` tag defined twice. |
| `E2015` | A field declared with function type. |
| `E2016` | `enum` with no tag name. |
| `E2017` | An enumerator list entry with no name. |
| `E2018` | `goto` with no label name. |
| `E2019` | A declaration was needed at file scope and something else was there. |
| `E2020` | `__interrupt_vector` with no handler name. |
| `E2021` | `{}` as an initializer. |
| `E2022` | (No longer reported: a trailing `,` in an initializer list is accepted.) |
| `E2023` | `__interrupt` on something that is not a function. |
| `E2024` | `inline` on something that is not a function. |
| `E2025` | `auto` on a function. |
| `E2026` | `register` on a function. |
| `E2027` | `inline` on a prototype rather than on a definition. |
| `E2028` | `__interrupt` combined with `inline`. |
| `E2029` | `...` with no named parameter before it. |
| `E2030` | `...` somewhere other than the end of a parameter list. |
| `E2031` | An array size that is not an integer constant. |
| `E2032` | An array size that is zero, negative or too large. |
| `E2033` | A declarator with no name in it. |
| `E2034` | A function declared to return a function. |
| `E2035` | A function declared to return an array. |
| `E2036` | An array of `void`, or of functions. |
| `E2037` | An array with no size where nothing can give it one: no initializer (and not an `extern` declaration), a scalar or flat list for an array of arrays, a struct member, a cast. Only a variable's initializer infers the outermost size, and `extern int t[];` may leave it unknown. |
| `E2038` | `_Static_assert(cond, x)` where `x` is not a string literal. |
| `E2039` | `__asm__(x)` after a declarator where `x` is not a string literal. |
| `E2040` | A designator that is not `.name` or `[index]`, or a `.` with no member name after it. |
| `E2041` | An `__attribute__` that is not well formed: missing parentheses, or no attribute name. |
| `E2042` | The argument of `aligned` is not a power of two from 1 to 65536 (or not a constant). |
| `E2043` | `__attribute__((packed))`: not supported, the machine faults on unaligned accesses. |
| `E2044` | `__asm__` with operands or clobbers (a `:` after the text). |
| `E2045` | `__asm__` text outside a function. |
| `E2046` | A `_Generic` association that starts with neither a type-name nor `default`. |

### `3xxx` — sema

| Code | Means |
| --- | --- |
| `E3001` | A name defined twice. |
| `E3002` | A label defined twice. |
| `E3003` | A name redefined as a different kind of thing. |
| `E3004` | A function defined twice. |
| `E3005` | Two declarations of one name that do not agree about its type. |
| `E3006` | Two declarations of one name that do not agree about its linkage. |
| `E3007` | A variable declared `void`. |
| `E3008` | An unnamed parameter in a definition, where the body could never refer to it. |
| `E3009` | An enumerator value that is not a constant expression. |
| `E3010` | A string literal initializing something other than an array of `char`. |
| `E3011` | A string literal that does not fit the array it initializes. |
| `E3012` | An array initialized by a plain expression. |
| `E3013` | An initializer whose type does not convert to the variable's. |
| `E3014` | An initializer list with the wrong number of values. |
| `E3015` | An initializer list missing the inner braces for a nested object. |
| `E3016` | An initializer for a type whose definition has not been seen. |
| `E3017` | A static object's initializer that is not a compile-time constant. |
| `E3018` | An initializer list somewhere other than a variable's initializer. |
| `E3019` | An `extern` declaration with an initializer inside a block. |
| `E3020` | `auto` at file scope. |
| `E3021` | `register` at file scope. |
| `E3022` | `register` on an array, whose every use takes its address. |
| `E3023` | A name that was never declared. |
| `E3024` | A `goto` to a label that does not exist. |
| `E3025` | A call with the wrong number of arguments. |
| `E3026` | An argument whose type does not convert to the parameter's. |
| `E3027` | An implicit integer/float conversion in a call argument. |
| `E3028` | A call through something that is not a function or a pointer to one. |
| `E3029` | `&` on something that is not an lvalue. |
| `E3030` | `&` on a `register` variable. |
| `E3031` | `*` on something that is not a pointer. |
| `E3032` | A unary operator applied to a type it does not take. |
| `E3033` | An assignment to something that is not an lvalue. |
| `E3034` | An assignment to a `const` object. |
| `E3035` | `++`/`--` on a type that has no such operation. |
| `E3036` | `&&`/`\|\|` on operands that are not scalars. |
| `E3037` | A comparison of two types that cannot be compared. |
| `E3038` | A binary operator applied to types it does not take. |
| `E3039` | `<<`/`>>` on operands that are not integers. |
| `E3040` | A compound assignment to a `struct`. |
| `E3041` | An assignment between incompatible types. |
| `E3042` | `[]` on something that is not a pointer or an array. |
| `E3043` | A subscript that is not an integer. |
| `E3044` | `->` on something that is not a pointer to a struct. |
| `E3045` | `.` on something that is not a struct. |
| `E3046` | A member name the struct does not have. |
| `E3047` | A cast between types with no conversion between them. |
| `E3048` | A condition that is not an arithmetic or pointer type. |
| `E3049` | A `?:` whose two arms have incompatible types. |
| `E3050` | `return value;` in a `void` function. |
| `E3051` | A returned value whose type does not convert to the return type. |
| `E3052` | `return;` in a function that has a return type. |
| `E3053` | `break` outside a loop or a `switch`. |
| `E3054` | `continue` outside a loop. |
| `E3055` | A `switch` on something that is not an integer. |
| `E3056` | `case` outside a `switch`. |
| `E3057` | A `case` label that is not an integer constant. |
| `E3058` | Two `case` labels with the same value. |
| `E3059` | `default` outside a `switch`. |
| `E3060` | Two `default` labels in one `switch`. |
| `E3061` | A `struct`/`union` passed through `...`. |
| `E3062` | A `void` value passed through `...`. |
| `E3063` | A variadic builtin operand that is not a `__builtin_va_list`. |
| `E3064` | `__builtin_va_start` outside a variadic function. |
| `E3065` | `__builtin_va_start` naming something other than the last fixed parameter. |
| `E3066` | `__builtin_va_arg` reading a type no variadic argument can be. |
| `E3067` | `&` on an `__interrupt` handler. |
| `E3068` | A call to an `__interrupt` handler. |
| `E3069` | An `__interrupt` handler that does not return `void`. |
| `E3070` | An `__interrupt` handler with parameters. |
| `E3071` | `main` declared `__interrupt`. |
| `E3072` | An interrupt number that is not a constant expression. |
| `E3073` | `__interrupt_vector(0, ...)` — the reset vector, which is what `main` already is. |
| `E3074` | An interrupt number outside 1–63. |
| `E3075` | `__interrupt_vector` naming something that is not a function. |
| `E3076` | `__interrupt_vector` naming a function not declared `__interrupt`. |
| `E3077` | Two `__interrupt_vector`s for one number. |
| `E3078` | A field declared `void`. |
| `E3079` | A field of an `enum` whose definition has not been seen. |
| `E3080` | A field of a `struct` whose definition has not been seen. |
| `E3081` | A `struct` that contains itself by value. |
| `E3082` | `sizeof` of an array whose size is not known in this unit (`extern int t[];`). |
| `E3083` | The condition of a `_Static_assert` is not a constant expression. |
| `E3084` | A `_Static_assert` whose condition is 0; the message, when there is one, is in the error. |
| `E3085` | An `__asm__("label")` on a declaration that is not at file scope. |
| `E3086` | An asm label that is not a plain identifier (letters, digits, `_`; not starting with a digit). |
| `E3087` | Two declarations of one name that give it different asm labels. |
| `E3088` | A designator in an initializer that does not fit what it initializes: `.x` for an array, `[i]` for a struct, or any designator for a scalar. A designator outside a brace list is the same code. |
| `E3089` | An array designator whose index is not an integer constant expression. |
| `E3090` | An array designator whose index is outside the array. |
| `E3091` | A designator for a member of a `union` other than the first. |
| `E3092` | A compound literal of type `void` or of a function type. |
| `E3093` | A `_Generic` selection with the same type in more than one association. |
| `E3094` | A `_Generic` selection with more than one `default` association. |
| `E3095` | A `_Generic` selection whose controlling expression's type matches no association, and there is no `default`. |
| `E3096` | A one-instruction builtin given an argument of the wrong bank (an integer builtin a float, or a float builtin an integer). |
| `E3097` | An empty `case` range (`case 5 ... 1:`), whose low bound is greater than its high bound. |
| `E3098` | A `case` range spanning more than 65536 values, which the dispatch cannot expand. |
| `E3099` | A flexible array member (`int a[];`) that is not a `struct`'s last field. |
| `E3100` | A struct with a flexible array member held by value: embedded in another struct, an array element, or an array object. |
| `E3101` | A flexible array member in a `union`, which has no tail for one to occupy. |

### `4xxx` — codegen

| Code | Means |
| --- | --- |
| `E4001` | Internal: a register-resident local's address reached code generation. |
| `E4002` | Internal: a call's arguments are not the `Param`s before it. |
| `E4003` | A global's initializer that is not a compile-time constant. |
| `E4004` | An `__asm__("label")` whose label is a reserved word in CeresASM (a C name that is one is no longer an error: it is written `__c_` and the name) — see [07-CASM-Interop.md](07-CASM-Interop.md). |
| `E4005` | Internal: a generated function with no declaration behind it. |
| `E4006` | A static initializer that is an address constant **with an offset** (like `&a[i]`) — see [06-Known-Limitations.md](06-Known-Limitations.md). |
| `E4007` | A C symbol named `__c_` and a reserved word of CeresASM: that is what the reserved word itself is written as — see [07-CASM-Interop.md](07-CASM-Interop.md). |

### `5xxx` — ir

| Code | Means |
| --- | --- |
| `E5001` | A compound assignment to a `struct` that reached lowering. |
| `E5002` | A 64-bit operation that is not lowered yet: multiplication, division, remainder, a shift, a `float`↔`long long` conversion, a 64-bit switch discriminant, or a 64-bit value crossing a function boundary. 64-bit add/sub/bitwise/comparisons/assignment all work — see [06-Known-Limitations.md](06-Known-Limitations.md). |

## Related pages

- [05-CLI.md](05-CLI.md) — `-Werror` and the rest of the command line.
- [08-Preprocessor.md](08-Preprocessor.md) — every other directive, and what `#pragma once` is for.
- [06-Known-Limitations.md](06-Known-Limitations.md) — what `W2001` is telling you about.

# The C subset

[← Back to index](README.md)

Ceres-C accepts a subset of C, not a dialect of it: everything it accepts means what it means in C.
What is left out is left out, not reinterpreted. This page is the contract — anything not described
here is, by definition, a syntax error.

## In scope

| Area | What is supported |
| --- | --- |
| Types | `void`, `bool`, `char`, `short`, `int`, `long`, `float`. `signed`/`unsigned` and `short`/`long` combine with `int`/`char` as in C. |
| Qualifiers | `const`, on either side of the type-spec and after a `*`. |
| Storage classes | `static`, `extern`, `auto` and the `inline` function specifier. |
| Derived types | Pointers, fixed-size arrays (1D and 2D), `struct`, `enum`, `typedef`. |
| Functions | Calls, recursion, up to any number of parameters, struct arguments and returns by value. |
| Statements | `if`/`else`, `while`, `do`/`while`, `for`, `switch`/`case`/`default`, `goto` + labels, `break`, `continue`, `return`. |
| Operators | All arithmetic, relational, logical (short-circuiting), bitwise, compound assignment, `&`, `*`, `[]`, `.`, `->`, `++`/`--` in both positions, `sizeof`, explicit casts. |
| Literals | Integers (decimal, `0x`, `0b`), floats (decimal and exponential), `char`, strings, `true`/`false`. |

`.` and `->` are genuinely different operators, not two spellings of one: the parser records which
token it saw and sema checks the operand accordingly. `p.x` needs a struct, `p->x` needs a pointer.

## Out of scope

| Left out | Why |
| --- | --- |
| `double` (f64) | The VM has no double-precision support at all. Supporting it would mean software emulation, not a type mapping. |
| `union`, bitfields | Nothing needs them yet, and each is a second layout rule to learn. |
| `volatile`, `restrict`, `register`, `alignof` | Reserved as keywords by the lexer so they can be rejected with a clear message, but not implemented. |
| Function pointers, varargs | Viable later — the ISA already has indirect calls. |
| Most of the preprocessor | `#include`, `#define` (object-like), `#undef` and `#pragma once` are implemented; `#if`/`#ifdef` and macros with arguments are not. See [08-Preprocessor.md](08-Preprocessor.md). |
| `malloc`/`free` | There is no allocator to call. |

The out-of-scope keywords exist as token kinds in the lexer on purpose: hitting one should produce
"not implemented in this version" rather than a generic syntax error that hides the fact that it is
a known, deliberate limit.

## const

`const` is a type **qualifier**, so it travels with the type rather than with the declaration — and
which side of a `*` it is written on decides what it qualifies:

```c
const int limit = 3;      // and `int const limit = 3;` - the same type
const char* text;         // a pointer to const char: *text = 'x' is an error
char* const cursor = buf; // a const pointer to char:  cursor = 0  is an error
const char* const both;   // neither
```

Writing to a const object is an error, and so is a conversion that would forget the qualifier
(`const int*` to `int*`). Adding it is always fine. A `const` global with an initializer is placed
in read-only memory, where the machine enforces it too.

## Storage classes

| Written | At file scope | Inside a block |
| --- | --- | --- |
| *(nothing)* | external linkage — visible to other objects | automatic storage |
| `static` | internal linkage — not published to the linker | storage that outlives the call |
| `extern` | a declaration, not a definition | names a file-scope object defined elsewhere |
| `auto` | **an error** — there is no automatic storage at file scope | automatic storage, i.e. the default |

`inline` is a function specifier rather than a storage class, so it combines with them
(`static inline`). Here it is a request the optimizer honours rather than a linkage rule: the
function is still emitted and still callable, and the inliner's size limit gives way for it whenever
inlining is on at all.

A `static` local's initializer becomes bytes in the loaded image, so it has to be a compile-time
constant — `static int n = someVariable;` is an error, `static int n = 1 + 2;` is not.

A name may be declared more than once at file scope as long as the declarations agree and at most
one has an initializer. That is exactly what a header's `extern` plus the defining source file is,
and it is why headers work.

## Grammar

The EBNF `libs/parser` implements. Uppercase names are token kinds from `libs/lexer`.

```ebnf
translation-unit       ::= external-decl*
external-decl          ::= function-def | declaration ";" | typedef-decl

function-def           ::= decl-specifier* type-name declarator
                            "(" param-list? ")" compound-stmt
param-list             ::= param ("," param)*
param                  ::= type-qualifier? type-name declarator

declaration            ::= decl-specifier* type-name init-declarator-list
init-declarator-list   ::= init-declarator ("," init-declarator)*
init-declarator        ::= declarator ("=" initializer)?
declarator             ::= "*"* direct-declarator
direct-declarator      ::= IDENTIFIER ("[" INT_LITERAL? "]")*
initializer            ::= assignment-expr | "{" initializer-list "}"
initializer-list       ::= initializer ("," initializer)*
typedef-decl           ::= "typedef" type-spec declarator ";"

decl-specifier         ::= storage-class-spec | type-qualifier   // any order, each at most once
storage-class-spec     ::= "static" | "extern" | "auto" | "inline"   // inline only on a function-def
type-qualifier         ::= "const"
type-name              ::= type-qualifier* type-spec type-qualifier*
                            ("*" type-qualifier*)*   // `const char*` vs `char* const`
sign-spec              ::= "signed" | "unsigned"
integer-type-spec      ::= sign-spec? ("char" | "short" "int"? | "int" | "long" "int"?)
                         | sign-spec "int"?          // signed/unsigned alone means int
type-spec              ::= "void" | "bool" | "float" | integer-type-spec
                         | struct-spec | enum-spec | IDENTIFIER   // IDENTIFIER: a typedef name
struct-spec            ::= "struct" IDENTIFIER ("{" member-decl+ "}")?
member-decl            ::= type-spec declarator ";"
enum-spec              ::= "enum" IDENTIFIER ("{" enumerator-list "}")?
enumerator-list        ::= enumerator ("," enumerator)*
enumerator             ::= IDENTIFIER ("=" INT_LITERAL)?

statement              ::= compound-stmt | if-stmt | while-stmt | do-stmt | for-stmt
                         | switch-stmt | goto-stmt | label-stmt
                         | break-stmt | continue-stmt | return-stmt
                         | decl-stmt | expr-stmt
compound-stmt          ::= "{" statement* "}"
if-stmt                ::= "if" "(" expression ")" statement ("else" statement)?
while-stmt             ::= "while" "(" expression ")" statement
do-stmt                ::= "do" statement "while" "(" expression ")" ";"
for-stmt               ::= "for" "(" (decl-stmt | expr-stmt | ";")
                            expression? ";" expression? ")" statement
switch-stmt            ::= "switch" "(" expression ")" "{" case-clause* default-clause? "}"
case-clause            ::= "case" INT_LITERAL ":" statement*
default-clause         ::= "default" ":" statement*
goto-stmt              ::= "goto" IDENTIFIER ";"
label-stmt             ::= IDENTIFIER ":" statement
break-stmt             ::= "break" ";"
continue-stmt          ::= "continue" ";"
return-stmt            ::= "return" expression? ";"
decl-stmt              ::= declaration ";"
expr-stmt              ::= expression? ";"

expression             ::= assignment-expr
assignment-expr        ::= conditional-expr (assign-op assignment-expr)?
assign-op              ::= "=" | "+=" | "-=" | "*=" | "/=" | "%="
                         | "&=" | "|=" | "^=" | "<<=" | ">>="
conditional-expr       ::= logical-or-expr ("?" assignment-expr ":" conditional-expr)?
logical-or-expr        ::= logical-and-expr ("||" logical-and-expr)*
logical-and-expr       ::= bit-or-expr ("&&" bit-or-expr)*
bit-or-expr            ::= bit-xor-expr ("|" bit-xor-expr)*
bit-xor-expr           ::= bit-and-expr ("^" bit-and-expr)*
bit-and-expr           ::= equality-expr ("&" equality-expr)*
equality-expr          ::= relational-expr (("==" | "!=") relational-expr)*
relational-expr        ::= shift-expr (("<" | "<=" | ">" | ">=") shift-expr)*
shift-expr             ::= additive-expr (("<<" | ">>") additive-expr)*
additive-expr          ::= multiplicative-expr (("+" | "-") multiplicative-expr)*
multiplicative-expr    ::= cast-expr (("*" | "/" | "%") cast-expr)*
cast-expr              ::= "(" type-name ")" cast-expr | unary-expr
unary-expr             ::= ("&" | "*" | "-" | "!" | "~" | "++" | "--") unary-expr
                         | "sizeof" ("(" type-name ")" | unary-expr)
                         | postfix-expr
postfix-expr           ::= primary-expr postfix-op*
postfix-op             ::= "[" expression "]" | "(" arg-list? ")"
                         | "." IDENTIFIER | "->" IDENTIFIER | "++" | "--"
primary-expr           ::= IDENTIFIER | INT_LITERAL | FLOAT_LITERAL | CHAR_LITERAL
                         | STRING_LITERAL | BOOL_LITERAL | "(" expression ")"
arg-list               ::= assignment-expr ("," assignment-expr)*
```

`storage-class-spec` and `type-qualifier` may appear in any order and any combination the language
itself allows, before the type-spec: `static const int`, `const static int` and `int const` all
parse, and two storage classes on one declaration (`static extern`) is reported as such.

## Operator precedence

Lowest to highest. Levels 2–11 are all left-associative binaries and are parsed by one
table-driven function rather than ten near-identical ones.

| Level | Operators | Associativity |
| --- | --- | --- |
| 1 | `=` `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` | right |
| 1½ | `?:` | right — the middle branch is a full assignment-expression, as in C |
| 2 | `\|\|` | left |
| 3 | `&&` | left |
| 4 | `\|` | left |
| 5 | `^` | left |
| 6 | `&` | left |
| 7 | `==` `!=` | left |
| 8 | `<` `<=` `>` `>=` | left |
| 9 | `<<` `>>` | left |
| 10 | `+` `-` | left |
| 11 | `*` `/` `%` | left |
| 12 | `(cast)` | right |
| 13 | prefix `&` `*` `-` `!` `~` `++` `--`, `sizeof` | right |
| 14 | postfix `[]` `()` `.` `->` `++` `--` | left |

## Type sizes and layout

| Type | Size | Alignment |
| --- | --- | --- |
| `char`, `bool` | 1 | 1 |
| `short` | 2 | 2 |
| `int`, `long`, `float`, any pointer | 4 | 4 |
| array | element size × count | the element's |
| struct | see below | the widest field's |

A struct follows the same rule CeresASM's own `struct` directive uses: every field is aligned to its
own size, and the total is rounded up to the widest field's alignment. The `examples/07_structs.c`
program prints the result for a deliberately awkward case.

```c
struct Padded { char tag; int value; char flag; };
//              ^0        ^4          ^8           sizeof == 12
```

## Errors

A syntax error does not stop the compilation of the file. The parser reports it and skips to the
next safe point — the next `;` or `}` inside a statement, the next type keyword at the top level —
so one run reports every error in a file instead of the first one. Sema does the same: it assumes
`int` for an expression it could not type and keeps checking.

Nothing reaches code generation if sema ended with at least one error. There is no point emitting
instructions for a program that does not type-check.

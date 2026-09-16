# The C subset

[← Back to index](README.md)

Ceres-C accepts a subset of C, not a dialect of it: everything it accepts means what it means in C.
What is left out is left out, not reinterpreted. This page is the contract — anything not described
here is, by definition, a syntax error.

## In scope

| Area | What is supported |
| --- | --- |
| Types | `void`, `bool`, `char`, `short`, `int`, `long`, `float`. `signed`/`unsigned` and `short`/`long` combine with `int`/`char` as in C. |
| Qualifiers | `const` as a type qualifier *(see the note on what the parser accepts today, below)*. |
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
| The preprocessor | `#include`/`#define` would be a text-to-text pass running before the lexer, not a token. |
| `malloc`/`free` | There is no allocator to call. |

The out-of-scope keywords exist as token kinds in the lexer on purpose: hitting one should produce
"not implemented in this version" rather than a generic syntax error that hides the fact that it is
a known, deliberate limit.

## What the parser accepts today

Two constructs are in the grammar below but are **not** accepted by the parser yet:

- `const` as a type qualifier, anywhere (`const int x = 3;`, `void f(const char* s)`).
- The storage-class specifiers `static`, `extern`, `auto` and `inline`.

Both are listed in the grammar because that is the contract the parser is meant to implement; the
gap is recorded in [06-Known-Limitations.md](06-Known-Limitations.md) with a reproducer. Nothing in
`examples/` uses either, so the shipped programs describe the language as it actually is.

## Grammar

The EBNF `libs/parser` implements. Uppercase names are token kinds from `libs/lexer`.

```ebnf
translation-unit       ::= external-decl*
external-decl          ::= function-def | declaration ";" | typedef-decl

function-def           ::= storage-class-spec? type-spec declarator
                            "(" param-list? ")" compound-stmt
param-list             ::= param ("," param)*
param                  ::= type-spec declarator

declaration            ::= storage-class-spec? type-qualifier? type-spec init-declarator-list
init-declarator-list   ::= init-declarator ("," init-declarator)*
init-declarator        ::= declarator ("=" initializer)?
declarator             ::= "*"* direct-declarator
direct-declarator      ::= IDENTIFIER ("[" INT_LITERAL? "]")*
initializer            ::= assignment-expr | "{" initializer-list "}"
initializer-list       ::= initializer ("," initializer)*
typedef-decl           ::= "typedef" type-spec declarator ";"

storage-class-spec     ::= "static" | "extern" | "auto" | "inline"   // inline only on a function-def
type-qualifier         ::= "const"
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
type-name              ::= type-spec "*"*        // for explicit casts and sizeof

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
assignment-expr        ::= logical-or-expr (assign-op assignment-expr)?
assign-op              ::= "=" | "+=" | "-=" | "*=" | "/=" | "%="
                         | "&=" | "|=" | "^=" | "<<=" | ">>="
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

There is no conditional operator (`?:`) in the subset — it is absent from the table above on
purpose, not by oversight.

## Operator precedence

Lowest to highest. Levels 2–11 are all left-associative binaries and are parsed by one
table-driven function rather than ten near-identical ones.

| Level | Operators | Associativity |
| --- | --- | --- |
| 1 | `=` `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` | right |
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

# The C subset

[← Back to index](README.md)

Ceres-C accepts a subset of C, not a dialect of it: everything it accepts means what it means in C.
What is left out is left out, not reinterpreted. This page is the contract — anything not described
here is, by definition, a syntax error.

## In scope

| Area | What is supported |
| --- | --- |
| Types | `void`, `bool`, `char`, `short`, `int`, `long`, `float`. `signed`/`unsigned` and `short`/`long` combine with `int`/`char` as in C. `long long`, `double` and `long double` are accepted and capped to 32 bits, with a warning. |
| Qualifiers | `const`, `volatile` and `restrict`, on either side of the type-spec and after a `*`. |
| Storage classes | `static`, `extern`, `auto`, `register` and the `inline` function specifier. |
| Derived types | Pointers, fixed-size arrays (1D and 2D), `struct`, `union`, `enum`, `typedef`, and function types — so function pointers, including arrays of them and functions that return them. |
| Functions | Calls (direct and through a pointer), recursion, up to any number of parameters, struct arguments and returns by value, and variadic `...` with `__builtin_va_list`/`__builtin_va_start`/`__builtin_va_arg`/`__builtin_va_end`/`__builtin_va_copy`. |
| Interrupts | `__interrupt` handlers and `__interrupt_vector`, plus `__builtin_sti`/`__builtin_cli`/`__builtin_halt` — see [10-Interrupts.md](10-Interrupts.md). |
| Statements | `if`/`else`, `while`, `do`/`while`, `for`, `switch`/`case`/`default`, `goto` + labels, `break`, `continue`, `return`. |
| Operators | All arithmetic, relational, logical (short-circuiting), bitwise, compound assignment, `&`, `*`, `[]`, `.`, `->`, `++`/`--` in both positions, `sizeof`, `alignof`, explicit casts. |
| Literals | Integers (decimal, `0x`, `0b`), floats (decimal and exponential), `char`, strings, `true`/`false`. Adjacent string literals are joined into one, as in C. |

`.` and `->` are genuinely different operators, not two spellings of one: the parser records which
token it saw and sema checks the operand accordingly. `p.x` needs a struct, `p->x` needs a pointer.

## Out of scope

| Left out | Why |
| --- | --- |
| 64-bit width — `long long`, `double`, `long double` | The spellings are accepted; the WIDTH is not. Ceres has no 64-bit register and no f64 register, so each one caps to its 32-bit counterpart with a warning. See [06-Known-Limitations.md](06-Known-Limitations.md). |
| Bitfields | A second layout rule to learn, and nothing needs them yet. `union` itself is supported. |
| Stringification (`#`), token pasting (`##`), `#line` | Everything else in the preprocessor is implemented, including `#if`/`#ifdef`, macros with arguments and the predefined `__LINE__`/`__FILE__` family. See [08-Preprocessor.md](08-Preprocessor.md). |
| `malloc`/`free` | There is no allocator to call. |

A capped type is a *spelling*, not a type of its own: `long long` and `long` are the same type
here, and so are `double` and `float`. That is what "there is no 64-bit anything" means once it is
followed through — a distinct type would be one no phase below the parser could represent.

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
external-decl          ::= declaration | typedef-decl | interrupt-vector-decl

// One production for a function and a variable alike. Which one it is falls out of the
// declarator: it declares a function when the derived type IS a function type, exactly as
// in C - `int f(int)` and `int (*f)(int)` differ in nothing else.
declaration            ::= decl-specifier* base-type declarator (("=" initializer)? ";" | compound-stmt)
param-list             ::= param ("," param)* ("," "...")?   // "..." only after a named parameter
                         | "void"                            // an explicitly empty list
param                  ::= "register"? base-type declarator   // the name may be omitted;
                                                              // "register" is the only
                                                              // storage class a parameter takes

// C's declarator, recursive. The suffixes bind tighter than the leading "*", which is what
// makes `int *f(void)` a function returning int* and `int (*f)(void)` a pointer to a
// function returning int. Parentheses are the only thing that tells the two apart.
declarator             ::= ("*" type-qualifier*)* direct-declarator
direct-declarator      ::= (IDENTIFIER | "(" declarator ")")? declarator-suffix*
declarator-suffix      ::= "[" INT_LITERAL? "]"      // size required except on a parameter
                         | "(" param-list? ")"
initializer            ::= assignment-expr | "{" initializer-list "}"
initializer-list       ::= initializer ("," initializer)*
typedef-decl           ::= "typedef" base-type declarator ";"
interrupt-vector-decl  ::= "__interrupt_vector" "(" constant-expr "," IDENTIFIER ")" ";"

decl-specifier         ::= storage-class-spec | type-qualifier | "__interrupt"
                                                     // any order, each at most once;
                                                     // __interrupt only on a function
storage-class-spec     ::= "static" | "extern" | "auto" | "register" | "inline"
                                                     // inline only on a function definition,
                                                     // register only on a local
type-qualifier         ::= "const" | "volatile" | "restrict"   // restrict only on a pointer
base-type              ::= type-qualifier* type-spec type-qualifier*
                                                     // `const int` and `int const` are one type
// A type-name is a declarator with the name left out - `int (*)(int)` is `int (*f)(int)`
// without the `f` - which is what a cast or `sizeof` needs.
type-name              ::= base-type declarator
sign-spec              ::= "signed" | "unsigned"
integer-type-spec      ::= sign-spec? ("char" | "short" "int"? | "int" | "long" "int"?)
                         | sign-spec "int"?          // signed/unsigned alone means int
type-spec              ::= "void" | "bool" | "float" | integer-type-spec
                         | struct-spec | union-spec | enum-spec | IDENTIFIER
                                                     // IDENTIFIER: a typedef name, or
                                                     // __builtin_va_list
struct-spec            ::= "struct" IDENTIFIER ("{" member-decl+ "}")?
union-spec             ::= "union" IDENTIFIER ("{" member-decl+ "}")?
member-decl            ::= base-type declarator ";"   // not a function: a struct holds objects
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
                         | "alignof" "(" type-name ")"
                         | postfix-expr
postfix-expr           ::= primary-expr postfix-op*
postfix-op             ::= "[" expression "]" | "(" arg-list? ")"
                         | "." IDENTIFIER | "->" IDENTIFIER | "++" | "--"
primary-expr           ::= IDENTIFIER | INT_LITERAL | FLOAT_LITERAL | CHAR_LITERAL
                         | STRING_LITERAL | BOOL_LITERAL | "(" expression ")"
                         | va-builtin | machine-builtin
                                                     // STRING_LITERAL is a RUN of one or more
                                                     // adjacent literals, joined by the lexer
arg-list               ::= assignment-expr ("," assignment-expr)*

// Syntax rather than calls: __builtin_va_arg's second operand is a type-name, and all four
// write through the __builtin_va_list the caller named. Only recognized when directly followed
// by "(" - see 09-Variadic-Convention.md.
va-builtin             ::= "__builtin_va_start" "(" assignment-expr "," IDENTIFIER ")"
                         | "__builtin_va_arg"   "(" assignment-expr "," type-name ")"
                         | "__builtin_va_end"   "(" assignment-expr ")"
                         | "__builtin_va_copy"  "(" assignment-expr "," assignment-expr ")"

// One machine instruction each, for the part of the machine no expression reaches -
// see 10-Interrupts.md.
machine-builtin        ::= ("__builtin_sti" | "__builtin_cli" | "__builtin_halt") "(" ")"
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

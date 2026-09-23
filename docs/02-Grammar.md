# The C subset

[← Back to index](README.md)

Ceres-C accepts a subset of C, not a dialect of it: everything it accepts means what it means in C.
What is left out is left out, not reinterpreted. This page is the contract — anything not described
here is, by definition, a syntax error.

## In scope

| Area | What is supported |
| --- | --- |
| Types | `void`, `bool`, `char`, `short`, `int`, `long`, `long long`, `float`. `signed`/`unsigned` and `short`/`long` combine with `int`/`char` as in C. `long long`/`unsigned long long` are real 8-byte types; `double` and `long double` are accepted and capped to `float`, with a warning. |
| Qualifiers | `const`, `volatile` and `restrict`, on either side of the type-spec and after a `*`. |
| Storage classes | `static`, `extern`, `auto`, `register` and the `inline` function specifier. |
| Derived types | Pointers, fixed-size arrays (1D and 2D; the size is an integer literal or arithmetic on literals, `[64]`, `[4 * 512]`, `[1 << 6]`, `[BUF + 8]` once macros expand; a variable's outermost size may be left out and taken from its initializer, `int a[] = {1, 2, 3}`, `char s[] = "hi"`, `int m[][2] = {{1, 2}, {3, 4}}`), `struct`, `union`, `enum`, `typedef`, and function types — so function pointers, including arrays of them and functions that return them. |
| Functions | Calls (direct and through a pointer), recursion, up to any number of parameters, struct arguments and returns by value, and variadic `...` with `__builtin_va_list`/`__builtin_va_start`/`__builtin_va_arg`/`__builtin_va_end`/`__builtin_va_copy`. |
| Interrupts | `__interrupt` handlers and `__interrupt_vector`, plus `__builtin_sti`/`__builtin_cli`/`__builtin_halt` — see [10-Interrupts.md](10-Interrupts.md). |
| Statements | `if`/`else`, `while`, `do`/`while`, `for`, `switch`/`case`/`default`, `goto` + labels, `break`, `continue`, `return`. |
| Operators | All arithmetic, relational, logical (short-circuiting), bitwise, compound assignment, `&`, `*`, `[]`, `.`, `->`, `++`/`--` in both positions, `sizeof`, `alignof`, explicit casts. |
| Literals | Integers (decimal, `0x`, `0b`), floats (decimal and exponential), `char`, strings, `true`/`false`. Adjacent string literals are joined into one, as in C. Integer literals take an optional `u`/`U` suffix (unsigned) and `ll`/`LL` (64-bit, in either order with `u`: `42ull`, `42llu`); float literals an optional `f`/`F`; `f`/`F` also forces a digit run to be a float (`1f`). |

`.` and `->` are genuinely different operators, not two spellings of one: the parser records which
token it saw and sema checks the operand accordingly. `p.x` needs a struct, `p->x` needs a pointer.

## Out of scope

| Left out | Why |
| --- | --- |
| 64-bit float — `double`, `long double` | The spellings are accepted; the WIDTH is not. Ceres has no f64 register, so each one caps to `float` with a warning. `long long`/`unsigned long long` are real 8-byte types but cannot yet be lowered to code — see [06-Known-Limitations.md](06-Known-Limitations.md). |
| Bitfields | A second layout rule to learn, and nothing needs them yet. `union` itself is supported. |
| Stringification (`#`), token pasting (`##`), `#line` | Everything else in the preprocessor is implemented, including `#if`/`#ifdef`, macros with arguments and the predefined `__LINE__`/`__FILE__` family. See [08-Preprocessor.md](08-Preprocessor.md). |
| `malloc`/`free` | There is no allocator to call. |

A capped type is a *spelling*, not a type of its own: `double` and `float` are the same type here.
That is what "there is no 64-bit float" means once it is followed through — a distinct type would be
one no phase below the parser could represent. `long long` is *not* capped: it is a distinct 8-byte
type, and only its lowering is still missing.

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

An `extern` array may leave its size out: `extern int table[];`, `extern char names[][4];` (only the
outermost size can be missing). It decays to a pointer and can be indexed like any array; what it cannot do is
be measured, because this unit does not know its size - `sizeof(table)` is E3082 - and a definition of the
same name in the unit, before or after, gives it its size (a different size or element type is E3005). Without
`extern`, and without an initializer to count, an array still needs its size (E2037).

### Designated initializers and the trailing comma

A brace list may name what an element is for, as in C99: `.field = v`, `[index] = v`, and paths of them
(`.pos.x = 1`, `[2].name = "a"`, `[1][2] = 5`). Positional elements after a designator continue from the next
member or index, whatever is not named is zero, and a later designation of the same place wins. The size of
`int a[] = { [5] = 1 }` is 6: the highest position reached plus one (which the parser can only count when the
index is a literal or arithmetic on literals). A trailing comma is accepted (`{ 1, 2, }`); `{}` still is not.

Sema rewrites such a list into the plain positional one it means - with a zero, or a list holding a zero, where
nothing was named - before anything else looks at it, so static and local initializers work unchanged. A member
that does not exist is E3046, a designator that does not fit what it initializes E3088, an index that is not
constant E3089 or outside the array E3090. Only the first member of a `union` can be designated (E3091), as it
is the only one a union initializer reaches at all.

### `__attribute__`

`__attribute__((name, name(args)))` is read wherever GCC allows one: among the declaration specifiers, between the
type and the declarator, after a declarator (in any order with `__asm__("label")`), on parameters, fields and
typedefs, after `struct`/`union` and after the closing brace, and as the statement `__attribute__((fallthrough));`.
`__name__` is the same as `name`.

What is done with them:

| Attribute | Effect |
| --- | --- |
| `noreturn` | Recorded on the function (a prototype's holds for its definition). A `return` inside such a function is warned about (W3002). No code is different: nothing here optimizes on it yet. |
| `noinline` | Recorded on the function. The inliner never splices it into its callers, however small it is. |
| `always_inline` | Recorded on the function. When inlining is on at all (`-O2`, or `-finline`), its size limit gives way entirely, so a body of any length is spliced in. The other inliner limits still apply: a body with control flow, a call, or a variadic parameter list is not inlined whatever it is marked, and no diagnostic is emitted for one that could not be. |
| `pure`, `const` | Recorded on the function. A call whose result nothing reads may be removed, like any other computation with no observable effect. `const` also promises the function reads no memory; `pure` only that it has no side effects. |
| `deprecated` | A call is warned about (W3003). Read wherever GCC allows it; a prototype's holds for its definition. |
| `warn_unused_result` | Discarding a call's result is warned about (W3004). A prototype's holds for its definition. |
| `aligned(N)` | `N` must be a power of two from 1 to 65536 (E2042). Up to 4 is what every scalar already has. Above 4 it is **ignored with a warning**: the linker puts every section on a 4-byte boundary, so a larger alignment could not be kept. |
| `packed` | **An error** (E2043): the machine faults on a 16- or 32-bit access that is not aligned, so a member cannot sit off its natural boundary. |
| `unused`, `used`, `fallthrough`, `cold`, `hot`, `nonnull`, `format`, `malloc`, `visibility`... | Accepted and dropped, silently: they tell a compiler how to check or optimize, and headers written for GCC are full of them. |
| any other | Accepted, and a warning says it is ignored (W2002), so a foreign header compiles. |

An attribute that would have an effect where none can (`noreturn` on a field or a parameter) is reported as ignored
rather than lost. A damaged one (`__attribute__(x)`, an unclosed list, a name that is not a name) is E2041.

### Inline assembly

`__asm__("text");` and `__asm__ volatile ("text");` are statements: the text goes into the function as written and is
treated as a call for register allocation - see [07-CASM-Interop.md](07-CASM-Interop.md#assembly-inside-a-c-function).
No operands or clobbers (E2044), and not outside a function (E2045).

### One-instruction builtins

A handful of machine instructions have no C expression, so they are spelled as builtins: a name in
call position, recognized the way `__builtin_sti` and the `va_*` builtins are, with the fixed arity
each one takes. Each lowers to exactly one Ceres instruction.

| Builtin | Instruction | Result |
| --- | --- | --- |
| `__builtin_clz(u)`, `__builtin_ctz(u)` | `clz`, `ctz` | `unsigned int` (`32` for a zero operand) |
| `__builtin_popcount(u)` | `popcnt` | `unsigned int` |
| `__builtin_bswap32(u)` | `bswap` | `unsigned int` |
| `__builtin_rotl32(u, n)`, `__builtin_rotr32(u, n)` | `rol`, `ror` | `unsigned int` |
| `__builtin_mulhu(a, b)`, `__builtin_mulhs(a, b)` | `mulh`, `imulh` | high 32 bits of the product |
| `__builtin_imin(a, b)`, `__builtin_imax(a, b)` | `imin`, `imax` | signed integer minimum / maximum |
| `__builtin_umin(a, b)`, `__builtin_umax(a, b)` | `min`, `max` | unsigned integer minimum / maximum |
| `__builtin_abs(i)` | `abs` | `int` |
| `__builtin_stack_pointer()` | `mov rd, sp` | `unsigned int`, the current stack pointer |
| `__builtin_fabs`, `__builtin_sqrt`, `__builtin_floor`, `__builtin_ceil`, `__builtin_trunc`, `__builtin_rint`, `__builtin_frcp`, `__builtin_frsqrt` | one float instruction each | `float` |
| `__builtin_fmod`, `__builtin_fmin`, `__builtin_fmax`, `__builtin_copysign` | one float instruction each | `float` |
| `__builtin_fclass(f)` | `fclass` | `int` classification bitmask |
| `__builtin_float_bits(f)` / `__builtin_float_from_bits(u)` | `mff` / `mtf` | raw bit reinterpretation |

A few compiler builtins are not a machine instruction of their own.
`__builtin_trap()` and `__builtin_unreachable()` both lower to the machine's `trap` (a point
promised never to be reached is read as a trap, which is the safe thing to do if it is).
`__builtin_expect(x, hint)` evaluates to `x` and emits nothing extra for the hint.
`__builtin_constant_p(e)` is `1` when `e` folds to a compile-time constant and `0` otherwise, and
does not evaluate `e` at all. `__builtin_offsetof` is not a builtin here - `<stddef.h>` provides it
as a macro.

`__builtin_add_overflow(a, b, &r)`, `__builtin_sub_overflow` and `__builtin_mul_overflow` store
`a op b` into `*r` (wrapping, as the machine's 32-bit arithmetic always did) and return `bool`: 1
when the signed or unsigned operation overflowed. Both operands must be 4-byte integers and the
third a pointer to one; the multiplication test uses the machine's multiply-high. A narrower
operand is rejected rather than silently widened.

`__builtin_rint` rounds ties to even, which is C's `rint`/`nearbyint`. `__builtin_abs` of
`INT_MIN` sets the machine's Overflow flag rather than producing a value, exactly as the instruction
does. The integer builtins take an integer and the float builtins a float, with no conversion applied
- pass the bank you mean. None is a function call and none clobbers memory, so a call whose result
nothing reads is removed like any other pure computation. There is no `__builtin_fma`: the machine's
`fma` accumulates into its destination, which this back end's two scratch float registers cannot
guarantee a spare register for.

### Compound literals

`(T){ ... }` is an unnamed object of type `T` set up from a brace list: `(struct P){ 1, 2 }`, `(struct P){ .y = 2 }`,
`(int[]){ 1, 2, 3 }` (whose size the list gives), `(int){ 4 }`. It is an lvalue, so `&(struct P){ 1, 2 }` and
`(struct P){ 1, 2 }.x = 5` work, and what follows it (`.m`, `[i]`, a call) applies to it. `(T)` followed by a
brace is a literal; followed by anything else it is a cast, as before.

Inside a function the object lives in the frame and is set up again each time the expression is evaluated, so a
literal in a loop starts from its list on every turn; its storage lasts as long as the function's frame, not just its
block. Outside a function it is a static variable (of a generated name, `__complitN`), so its values must be
constants and its address is one too: `int* a = (int[]){ 1, 2, 3 };` works. Sema checks the list exactly as it
checks the initializer of a variable of that type, designators included.

### `_Static_assert` and `__func__`

`_Static_assert(condition, "message");` is checked while compiling and produces no code. The condition is an
integer constant expression - the same kind a static initializer takes: enumerators, `sizeof` of any complete
type, arithmetic and comparisons. A false one is E3084 carrying the message, a non-constant one E3083. It may
stand at file scope, in a block, and in a struct body (where it is checked just after the struct, once its layout
is known). The message is optional, as in C23; `assert.h` in the standard library spells it `static_assert`.

`__func__` (and `__FUNCTION__`) inside a function is the function's name, as a string literal - so, unlike in
C, it has the type of one (`char*`) and `sizeof(__func__)` is the size of a pointer. Outside a function it is an
ordinary identifier, and undeclared.

### `_Generic`

`_Generic(controlling, type1: expr1, ..., default: exprN)` picks one association by the controlling expression's
type - compared as a type, the same notion of "same type" everything else in sema uses, not by value - and the
whole expression has that association's type and value. Nothing about the controlling expression's *value* is
used, and it is never evaluated (`_Generic(x++, int: 1, default: 2)` never increments `x`, the same way
`sizeof(x++)` never does), but its type still has to be resolved, which does mean checking it. Every association's
expression is type-checked whether or not it is the one picked - only evaluation is skipped, not checking - so
`_Generic(1, int: 1, float: undeclared)` is still an error even though the `float:` branch never runs.

At most one `default:` association, and at most one association per type (E3093/E3094); with no `default` and no
match, E3095. A type-name may be anything [`type-name`](#grammar) accepts, pointer types included, so
`_Generic(p, int*: 1, char*: 2, default: 3)` tells two pointer types apart. Because only the *selected*
association is ever lowered to code, the safe way to write one is to make every association a bare function name
- never a call - and apply the call once, outside the selection: `_Generic(x, int: f_int, float: f_float)(x)`.
A call written *inside* an association is still type-checked even when that association is not the one picked
(see above), so a call whose arguments only make sense for one branch's type belongs outside the selection, not in it.

## Grammar

The EBNF `libs/parser` implements. Uppercase names are token kinds from `libs/lexer`.

```ebnf
translation-unit       ::= external-decl*
external-decl          ::= declaration | typedef-decl | interrupt-vector-decl

// One production for a function and a variable alike. Which one it is falls out of the
// declarator: it declares a function when the derived type IS a function type, exactly as
// in C - `int f(int)` and `int (*f)(int)` differ in nothing else. A base type may introduce
// several declarators (`int a, b, c;`), each with its own `*`/`[...]`/initializer; a function
// DEFINITION (a declarator followed by a body) ends the declaration by itself, so no ';' and
// no further declarator follow it.
declaration            ::= decl-specifier* base-type init-declarator-list? ";"
init-declarator-list   ::= init-declarator ("," init-declarator)*
init-declarator        ::= declarator ("=" initializer)?     // or, naming a function:
                          | declarator compound-stmt          //   a definition: no ';' and no ','
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
initializer            ::= assignment-expr | "{" initializer-list ","? "}"
initializer-list       ::= (designation? initializer) ("," designation? initializer)*
designation            ::= designator+ "="
designator             ::= "." IDENTIFIER | "[" constant-expr "]"
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
                         | "struct" "{" member-decl+ "}"       // no tag: `typedef struct { ... } T;`
union-spec             ::= "union" IDENTIFIER ("{" member-decl+ "}")?
                         | "union" "{" member-decl+ "}"
member-decl            ::= base-type declarator ("," declarator)* ";"   // not a function: a struct holds objects
enum-spec              ::= "enum" IDENTIFIER ("{" enumerator-list "}")?
                         | "enum" "{" enumerator-list "}"       // no tag: `enum { A, B };`
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
case-clause            ::= "case" constant-expr ("..." constant-expr)? ":" statement*
                                                    // `case low ... high:` (GNU) covers every value in the
                                                    // range; both bounds must be constants, low <= high, and
                                                    // the range may not span more than 65536 values
default-clause         ::= "default" ":" statement*
goto-stmt              ::= "goto" IDENTIFIER ";"
label-stmt             ::= IDENTIFIER ":" statement
break-stmt             ::= "break" ";"
continue-stmt          ::= "continue" ";"
return-stmt            ::= "return" expression? ";"
decl-stmt              ::= declaration
expr-stmt              ::= expression? ";"

expression             ::= assignment-expr ("," assignment-expr)*    // the comma operator: evaluate the left, yield the right
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
unary-expr             ::= ("&" | "*" | "-" | "!" | "~") cast-expr   // so `*(T*)p` and `-(int)x` need no extra parentheses
                         | ("++" | "--") unary-expr
                         | "sizeof" ("(" type-name ")" | unary-expr)
                         | "alignof" "(" type-name ")"
                         | postfix-expr
postfix-expr           ::= primary-expr postfix-op*
postfix-op             ::= "[" expression "]" | "(" arg-list? ")"
                         | "." IDENTIFIER | "->" IDENTIFIER | "++" | "--"
primary-expr           ::= IDENTIFIER | INT_LITERAL | FLOAT_LITERAL | CHAR_LITERAL
                         | STRING_LITERAL | BOOL_LITERAL | "(" expression ")"
                         | va-builtin | machine-builtin | generic-selection
                                                     // STRING_LITERAL is a RUN of one or more
                                                     // adjacent literals, joined by the lexer
arg-list               ::= assignment-expr ("," assignment-expr)*

// "_Generic" is recognized by name plus a following "(", the same way "_Static_assert" is -
// see the prose above and 06-Known-Limitations.md for why neither needs a token kind of its own.
generic-selection      ::= "_Generic" "(" assignment-expr "," generic-assoc-list ")"
generic-assoc-list     ::= generic-assoc ("," generic-assoc)*
generic-assoc          ::= (type-name | "default") ":" assignment-expr

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
| 0 | `,` | left — only where a whole expression is wanted (a statement, a condition, a `for` clause, parentheses); an argument, an initializer or an enumerator is an assignment-expression, so a comma there still separates |
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
| `long long`, `unsigned long long` | 8 | 8 |
| array | element size × count | the element's |
| struct | see below | the widest field's |

A struct follows the same rule CeresASM's own `struct` directive uses: every field is aligned to its
own size, and the total is rounded up to the widest field's alignment. The `examples/07_structs.c`
program prints the result for a deliberately awkward case.

```c
struct Padded { char tag; int value; char flag; };
//              ^0        ^4          ^8           sizeof == 12
```

`long long` and `unsigned long long` are real 8-byte types: `sizeof`, struct layout and an `ll`/`LL`
literal suffix all see the full width, and a value lowers as an addressed pair of words, so
addition, subtraction, multiplication, division, remainder, the bitwise operators, comparisons and
assignment compute on all 64 bits. Shifts and the float conversions, and passing a 64-bit value
across a function boundary, are not implemented yet — a program that uses one of those is refused
with `E5002` rather than silently truncated. See [06-Known-Limitations.md](06-Known-Limitations.md).

## Errors

A syntax error does not stop the compilation of the file. The parser reports it and skips to the
next safe point — the next `;` or `}` inside a statement, the next type keyword at the top level —
so one run reports every error in a file instead of the first one. Sema does the same: it assumes
`int` for an expression it could not type and keeps checking.

Nothing reaches code generation if sema ended with at least one error. There is no point emitting
instructions for a program that does not type-check.

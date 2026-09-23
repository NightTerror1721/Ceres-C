# Ceres-C: análisis completo del compilador, la máquina y la biblioteca estándar, con propuestas

> Informe de investigación (revisión 2). Auditoría del compilador Ceres-C, de su máquina objetivo
> (CeresASM / VM Ceres) y de la **biblioteca estándar Ceres STDLIB**, con
> **14 propuestas de nuevas características** y **15 propuestas de mejoras u optimizaciones**.
>
> Fecha del análisis: 2026-09-22.
> Base:
> - compilador: `D:\Projects\Ceres-C`
> - VM + ensamblador: `D:\Projects\CeresASM\Ceres` y `D:\Projects\CeresASM\docs`
> - biblioteca estándar: `D:\Projects\Ceres Projects\Ceres STDLIB`
>
> Todo se contrasta contra el **código real**. Cuando código y documentación discrepan, se dice
> explícitamente (§14). Esta revisión **corrige** el error de la revisión 1, que afirmaba que no
> existía biblioteca estándar: sí existe, es un proyecto aparte, y se analiza en §10.

---

## Índice

1. [Resumen ejecutivo](#1-resumen-ejecutivo)
2. [Metodología y fuentes](#2-metodología-y-fuentes)
3. [El ecosistema: tres proyectos](#3-el-ecosistema-tres-proyectos)
4. [Arquitectura del compilador](#4-arquitectura-del-compilador)
5. [El pipeline etapa por etapa](#5-el-pipeline-etapa-por-etapa)
6. [El lenguaje que acepta](#6-el-lenguaje-que-acepta)
7. [La máquina objetivo: ISA Ceres](#7-la-máquina-objetivo-isa-ceres)
8. [Back-end: IR, optimizador, registros, frames y ABI](#8-back-end-ir-optimizador-registros-frames-y-abi)
9. [Capacidades avanzadas ya presentes](#9-capacidades-avanzadas-ya-presentes)
10. [La biblioteca estándar Ceres STDLIB](#10-la-biblioteca-estándar-ceres-stdlib)
11. [Lo que el compilador NO usa de la máquina (y cómo lo suple la STDLIB)](#11-lo-que-el-compilador-no-usa-de-la-máquina-y-cómo-lo-suple-la-stdlib)
12. [Propuestas de nuevas características (F1–F14)](#12-propuestas-de-nuevas-características-f1f14)
13. [Propuestas de mejoras y optimizaciones (O1–O15)](#13-propuestas-de-mejoras-y-optimizaciones-o1o15)
14. [Desincronización documentación ↔ código](#14-desincronización-documentación--código)
15. [Matriz de esfuerzo/impacto y roadmap sugerido](#15-matriz-de-esfuerzoimpacto-y-roadmap-sugerido)

---

## 1. Resumen ejecutivo

El ecosistema Ceres son **tres proyectos independientes**:

1. **CeresASM / VM Ceres** — la máquina: ISA de 32 bits, ensamblador CASM, linker, depurador, MMU y
   dispositivos MMIO.
2. **Ceres-C** — el compilador: un subconjunto estricto de C en C++23 que emite **texto CASM legible**,
   invocando `ceres` como subproceso (nunca enlaza contra él).
3. **Ceres STDLIB** — la biblioteca estándar: 88 encabezados y 74 fuentes C (~10 000 líneas) más 8
   ficheros CASM escritos a mano, que dan a los programas `<stdio.h>`, `<stdlib.h>`, `<string.h>`,
   `<math.h>`, `<setjmp.h>`, `<signal.h>`, `<time.h>`, un sistema de ficheros (CeresFS), contenedores,
   gráficos, TUI, audio y acceso a dispositivos.

**El compilador está maduro en el front-end y correcto en el back-end**, pero hay dos hechos que
gobiernan este informe:

- **La STDLIB existe y es seria.** La revisión 1 la ignoró; §10 la documenta por completo. Cualquier
  propuesta debe partir de que un programa real ya se escribe así:
  `ceresc prog.c build/lib/O2/libceres.car --decls … -I include --run`.
- **La STDLIB suple con CASM escrito a mano lo que el compilador no alcanza.** `asm/bits.casm` y
  `asm/math_ops.casm` existen porque C "no tiene forma de deletrear" `clz`/`popcnt`/`bswap`/`rol`,
  `fclass`/`mff`/`sqrt`/`fma`/`floor`/…; `asm/sys.casm` lee `sp` y los flags; `asm/setjmp.casm`
  codifica a mano el conjunto de registros callee-saved. Ese es el mapa de vacíos del compilador.

Además, la propia STDLIB **mide** la calidad del código generado: `asm/string_fast.casm` documenta que
sus rutinas palabra-a-palabra son ~3× más rápidas que las mismas escritas en C a `-O2`
(`strcpy` 11305 vs 32797 instrucciones, `strcmp` 13356 vs 40995, `strchr` 15407 vs 45089). Eso motiva
directamente la propuesta O15 (reconocimiento de idiomas de bucle).

Las propuestas se agrupan en tres olas:

- **Ola A — integrar y cerrar vacíos** (F1 builtins de ISA, F5 integración de la STDLIB en el driver,
  F8 debug info de C, F11 atributos, O15 idiomas de bucle, O1 reducción de fuerza).
- **Ola B — calidad de código** (O2 CSE, O3 bucles, O4 registros, O5 copias, O8 PC-relativo, F13 LTO,
  O13 niveles).
- **Ola C — profundidad de lenguaje** (F2/F3 aritmética y 64 bits, F4 setjmp nativo, F6/F7 alloca y
  bitfields, O10/O11/O14).

---

## 2. Metodología y fuentes

- **Documentación Ceres-C** (`docs/01`–`docs/13`, `README.md`).
- **Código Ceres-C**: las 9 bibliotecas de `libs/`, `apps/ceresc`, `tests/`, `examples/`.
- **Documentación CeresASM** (`D:\Projects\CeresASM\docs`, 29 documentos) y **código**: `libs/core`
  (ISA, registros, opcodes, debug info), `libs/vm`, `libs/asm`, `libs/devices`, `libs/debug`.
- **Ceres STDLIB**: `README.md`, `Makefile`, `tools/*.ps1`, los 88 encabezados de `include/`, las 74
  fuentes de `src/`, los 8 ficheros de `asm/`, los 15 `examples/` y los 74 `tests/`.
- Verificación de símbolos concretos con lectura directa de fuentes: `ir_instr.h`,
  `ir_optimizer.cpp`, `value_placement.cpp`, `codegen.h`, `optimization.h`, `opcodes.h`,
  `registers.h`, `execution_engine.h`, `asm/bits.casm`, `asm/math_ops.casm`, `asm/setjmp.casm`,
  `src/malloc.c`, `src/exit.c`, `src/ceres/fault.c`, `include/ceres/*.h`.

---

## 3. El ecosistema: tres proyectos

```
┌────────────────────┐    escribe .casm     ┌────────────────────┐
│      Ceres-C       │ ───────────────────▶ │     CeresASM       │
│  (compilador C)    │                      │  asm · link · run  │
│  C++23, 9 libs     │ ◀── subproceso ───── │  CASM · VM · debug │
└─────────┬──────────┘    `ceres`           └────────────────────┘
          │                                         ▲
          │  compila fuentes .c de la STDLIB        │  enlaza .cobj / .car
          ▼                                         │
┌──────────────────────────────────────────────────┴───────────────┐
│                        Ceres STDLIB                              │
│  src/**.c (74)  asm/*.casm (8)  include/**.h (88)  tests (74)     │
│  → build/lib/O{0,1,2}/libceres.car + libceres.decls.casm          │
└──────────────────────────────────────────────────────────────────┘
```

La dependencia es **unidireccional por capas**: la STDLIB necesita a Ceres-C y a CeresASM; Ceres-C solo
necesita a CeresASM (y solo como subproceso, y solo con `--run`).

**Construcción de la STDLIB** (`tools/mklib.ps1`):

1. `ceresc <todos los src/**/*.c> -I include -O<level> -Werror -S` → `.casm` por unidad + un fichero
   `libceres.decls.casm` con las declaraciones.
2. `ceres asm -c` ensambla cada `.casm` (generado y a mano) a `.cobj`.
3. `ceres ar` los recolecta en `libceres.car`. **Los miembros de un archivo solo se enlazan si
   responden a un nombre que nada anterior define**, lo que es el mecanismo con el que los módulos que
   *bindean vectores de interrupción* (`src/ceres/irq.c`, `asm/optional/fault.casm`) quedan fuera salvo
   que un programa los pida.
4. Verificación: dos programas se construyen contra el archivo y se ejecutan comparando byte a byte.

**Uso por un programa** (`tools/common.ps1::Get-LibraryArgs`):

```
ceresc prog.c build/lib/O2/libceres.car --decls build/lib/O2/libceres.decls.casm -I include -O2 --run
```

**Tests de la STDLIB** (`tools/runtests.ps1`): cada test se compila a `-O0`, `-O1` y `-O2`, se enlaza
con la biblioteca del mismo nivel, se ejecuta y su salida se compara **byte a byte**, exigiendo que los
tres niveles impriman lo mismo. Es la misma filosofía de bisección que los `examples` de Ceres-C.

---

## 4. Arquitectura del compilador

```
file.c ─▶ preprocessor ─▶ lexer ─▶ parser ─▶ sema ─▶ IR ─▶ optimizador ─▶ codegen ─▶ file.casm
                                                                                        │
                                          ceres asm -c ─▶ ceres link ─▶ ceres run ◀─────┘
```

Nueve bibliotecas, cada una con una única responsabilidad:

| Biblioteca | Rol | Dato clave |
| --- | --- | --- |
| `libs/support` | `SourceManager`, `DiagnosticEngine`, `Arena`, `LineMap`, `StringPool`, `OptimizationOptions` | Base de todas; `optimization.h` es la tabla única de flags |
| `libs/preprocessor` | `#include`/`#define`/condicionales, mapa de líneas | Construye el `LineMap` que hace honestos diagnósticos y comentarios |
| `libs/lexer` | Caracteres → tokens | Maximal munch |
| `libs/ast` | Árbol y **sistema de tipos** (solo datos) | `Type` con payload por variante |
| `libs/parser` | Tokens → árbol | Resincroniza: reporta varios errores por fichero |
| `libs/sema` | Árbol → árbol anotado | Tipos, símbolos, layout, valores constantes |
| `libs/ir` | Árbol → IR de 3 direcciones + optimizador | Sin SSA, sin dominadores; bloques lineales |
| `libs/codegen` | IR → texto CASM | Único punto que conoce la ABI, la ISA y `.casm` |
| `libs/driver` | Pipeline, CLI, subprocesos | `--run`, `--decls`, `--emit-decls`, limpieza |

Conteo: 4301 símbolos, 11666 relaciones, 198 flujos (GitNexus). Tamaños: `parser.cpp` 2433 líneas,
`codegen.cpp` 2099, `sema.cpp` 2045, `ir_builder.cpp` 1956, `ir_optimizer.cpp` 1460.

---

## 5. El pipeline etapa por etapa

### 5.1 Preprocesador

Directivas: `#include "..."`/`<...>`, `#define` (objeto, función y variádico con `__VA_ARGS__`),
`#`/`##`, `#undef`, `#if`/`#elif`/`#else`/`#endif`, `#ifdef`/`#ifndef`, `#error`, `#warning`,
`#pragma once`, `#pragma warning(...)`, y continuación de línea con `\`.

Macros predefinidas: `__LINE__`, `__FILE__`, `__DATE__`, `__TIME__`, `__STDC__`, `__STDC_HOSTED__`(=0),
`__CERESC__`, `__BASE_FILE__`, `__INCLUDE_LEVEL__`, `__COUNTER__`.

**No hay `#line`**: en su lugar, un `LineMap` mapea cada línea expandida a su (fichero, línea)
original. Ese mapa lo usan el impresor de diagnósticos y `libs/codegen` para los comentarios
`// file:line`. Es el mecanismo que hace que un error en `include/stdio.h` de la STDLIB señale
`stdio.h:110`, no una línea del buffer expandido.

### 5.2 Lexer y parser

Lexer con maximal munch; el parser implementa la EBNF de `docs/02-Grammar.md` con una tabla de
precedencias y recuperación de errores. La STDLIB ejercita el parser a fondo: macros con `##`
(`IRQ_STUB` en `src/ceres/irq.c`), `switch` grandes, `union` (para bits de float en `src/format.c`),
punteros a función (`qsort`, `signal`, `atexit`, `irq_attach`), structs por valor (`ns64`, `div_t`).

### 5.3 Sema

Anota tipos, resuelve nombres, calcula layout, evalúa constantes, reescribe designadores y valida
conversiones, cualificadores, linkage, inicializadores estáticos, `_Static_assert`, `_Generic`,
`__attribute__`, literales compuestos, varargs, `__interrupt` y vectores. La STDLIB usa `_Static_assert`
(`ceres/ns64.h`) y `union` con solapamiento de tipos.

### 5.4 IR, optimizador y codegen

Se detallan en §8.

### 5.5 Driver

Compila cada `.c`, escribe `.casm`, genera un fichero de declaraciones compartido, ensambla, enlaza y
ejecuta como subprocesos. Acepta `.casm`, `.cobj` y `.car`: exactamente lo que la STDLIB produce y
consume.

---

## 6. El lenguaje que acepta

### 6.1 En alcance

| Área | Detalle |
| --- | --- |
| Tipos | `void`, `bool`, `char`, `short`, `int`, `long`, `long long`, `float`; `signed`/`unsigned`; `long long`/`unsigned long long` **reales de 8 bytes** (F3.1a, sin bajar aún); `double`/`long double` **aceptados y recortados a `float` con aviso W2001** |
| Cualificadores | `const`, `volatile`, `restrict` |
| Almacenamiento | `static`, `extern`, `auto`, `register`, `inline` |
| Derivados | Punteros, arrays 1D/2D fijos (con deducción desde inicializador), `struct`, `union`, `enum`, `typedef`, tipos función |
| Funciones | Recursión, cualquier número de parámetros, structs por valor, variádicas |
| Interrupciones | `__interrupt`, `__interrupt_vector`, `__builtin_sti/cli/halt` |
| Sentencias | `if/else`, `while`, `do/while`, `for`, `switch/case/default`, `goto`+etiquetas, `break`, `continue`, `return` |
| Operadores | Todos, incluidos `&&`/`\|\|`, asignación compuesta, `++`/`--`, `sizeof`, `alignof`, casts, `?:`, `.` y `->` distintos, y el **operador coma** |
| Literales | Enteros (`0x`, `0b`), flotantes, `char`, cadenas adyacentes unidas, `true`/`false`, sufijos `u`, `ll`/`LL` y `f` |
| Extensiones | `__attribute__((...))`, `__asm__("...")`, literales compuestos, `_Static_assert`, `__func__`, `_Generic`, designadores, coma final |

### 6.2 Fuera de alcance (del compilador)

- **Anchos de 64 bits bajados a código**: `long long`/`unsigned long long` son ya tipos reales de 8
  bytes (F3.1a: `sizeof`, layout, literales `ll`/`LL`) y su representación como pareja `(lo, hi)`,
  aritmética, shifts, conversiones y ABI ancha están bajadas (F3.1b–F3.4); un valor de 64 bits ya no se
  rechaza con `E5002`. Quedan fuera, por caer en otra frontera, un discriminante de `switch` ancho
  (F9) y un operando ancho de un builtin de una instrucción (F3.3). `double`/`long double` se recortan
  a `float` (W2001); sin `l`/`L` en literales. La STDLIB lo asume explícitamente
  (`include/stdint.h`: "Nothing 64-bit is defined… an int64_t that quietly held 32 would be a trap") y
  se simplificará en F3.5.
- **Bitfields**, `__attribute__((packed))` (E2043).
- **`#line`**, `#include_next`, `_Pragma`, `__has_include`, `#embed`.
- **Dirección constante con desplazamiento** (`&a[i]` en inicializador estático) → `E4006`.
- **`__asm__` con operandos o clobbers** (E2044).
- **División por cero no falla** (la VM pone el flag Trap; `signal(SIGFPE)` puede activar el fallo).

### 6.3 Diagnósticos

Códigos estables por fase (`0xxx` lexer, `1xxx` preprocesador, `2xxx` parser, `3xxx` sema, `4xxx`
codegen, `5xxx` IR). `#pragma warning(disable|enable|default|error|push|pop: N)` y `-Werror`, con el
pragma ganando en ambos sentidos. La STDLIB se compila con `-Werror`, así que **todo warning del
compilador es, en la práctica, un error de build de la biblioteca**: es un banco de pruebas exigente.

> **Corrección respecto a la revisión 1:** no es cierto que "no haya biblioteca estándar". El
> compilador *no la incluye ni la conoce*, pero el proyecto Ceres STDLIB (§10) provee una biblioteca
> estándar completa y una librería de plataforma. `docs/06-Known-Limitations.md` de Ceres-C ("No
> standard library") está desactualizado a nivel de ecosistema.

---

## 7. La máquina objetivo: ISA Ceres

Máquina de 32 bits, sin 64 bits ni f64. Memoria plana (16 MiB por defecto, 1 GiB máximo), MMIO en
`0xFF000000`+, 64 vectores de interrupción, pila hacia abajo, `.text` de solo lectura, MMU de 2 niveles
disponible pero no usada por el compilador.

### 7.1 Registros

| Banco | Registros | Notas |
| --- | --- | --- |
| Enteros | `r0`–`r12` | `r0`–`r3` argumentos/retorno |
| `r13`/`at` | Assembler temporary | **Lo pisa el ensamblador** (`stv`, `ldv` flotante lejano). No asignable |
| `r14`/`fp` | Frame pointer | gestionado por `enter`/`leave` |
| `r15`/`sp` | Stack pointer | |
| Flotantes | `f0`–`f15` | `f0`–`f3` argumentos; `f0` retorno |

Convención (no impuesta por hardware): `r0`–`r7`, `r12`, `f0`–`f7`, flags y `at` *caller-saved*;
`r8`–`r11`, `f8`–`f15` *callee-saved*. La VM no salva nada en `call`/`ret`/`enter`/`leave`.

### 7.2 Instrucciones destacables

- **Aritmética con acarreo**: `ADDC`, `ADDCI`, `SUBC`, `SUBCI`.
- **Multiplicación alta**: `MULH`, `IMULH`.
- **Bitwise/bit-count**: `CLZ`, `CTZ`, `POPCNT`, `BSWAP`, `ROL`, `ROR`, `NOT`.
- **Min/abs**: `ABS`, `MIN`/`IMIN`/`MAX`/`IMAX` (+ inmediatas).
- **Saltos comparativos de dos flags** (`JGR/JGE/JLS/JLE`, `JAB/JAE/JBL/JBE`) y **overflow** (`JO/JNO`).
- **Branch-and-link**: `BL`/`BLR`.
- **Carga/almacén PC-relativo**: `LDRP`/`STRP` (±32 KiB).
- **Indexado**: `LDRX`/`STRX` (sí usados, vía address-folding).
- **Flotante extendido**: `FSQRT`, `FABS`, `FMOD`, `FMIN`, `FMAX`, `FROUND`, `FFLOOR`, `FCEIL`,
  `FTRUNC`, `FCOPYSIGN`, `FMA`, `FCLASS`, `FRECIPE`, `FRSQRTE`, `MTF`, `MFF`, `ITOF`, `FTOI`.
- **MMU**: `MTP`, `MFP`, `PGON`, `PGOFF`, `INVLPG`, `FLPG`, `MFPF`.
- **Sistema**: `TRAP`, `INT imm8`, `IRET`.

### 7.3 Dispositivos MMIO

Terminal (`0xFF000000`), Timer, Disco, Framebuffer de texto, DMA, Teclado, Ratón, Display de píxeles,
Gamepad, Audio, Puertos periféricos y System control (`0xFFFF0000`, con el **estado de salida en bits
15:8**). La STDLIB expone todos ellos en `include/ceres/*.h`.

### 7.4 Información de depuración (CeresASM)

`ceres asm --debug` produce una **tabla de líneas** y una **tabla de símbolos** serializadas en el
`.cres` (bloque CDBG), con consultas `locationOf`, `addressesOf`, `symbolNamed`. El depurador las
consume. Ceres-C **no** emite nada de esto hoy (§12 F8), aunque `include/ceres/sys.h` de la STDLIB ya
anticipa el beneficio: el reportero de fallos imprime un PC crudo y comenta "with `--debug` the address
can be turned into file:line".

---

## 8. Back-end: IR, optimizador, registros, frames y ABI

### 8.1 IR

`IrFunction` = lista de `BasicBlock`, cada uno lineal terminado en salto/retorno. Sin SSA ni
dominadores. Valores = temporales virtuales, nunca registros físicos. Opcodes (`ir_instr.h`): `Const`,
`BinOp`, `UnOp`, `Cmp`, `Copy`, `FrameAddr`, `GlobalAddr`, `Load`, `Store`, `Param`, `Call`, `VaStart`,
`Jump`, `CondJump`, `TableJump`, `Return`, `MachineOp`.

Invariante clave: **un valor de tipo estrecho siempre está ya estrechado** (`Narrow`/`ToBool`), lo que
hace correcto el forwarding de store a load.

### 8.2 Optimizador (`optimize()`)

1. `inlineCalls` (solo `inlining`, de `-O2`).
2. Punto fijo (máx. 8 rondas) por función: `foldFunction` → `forwardLoads` → `forwardRestrictLoads` →
   `propagateCopies` → `eliminateDeadStores` → `threadJumps` → `removeUnreachableBlocks` →
   `eliminateDeadCode`.
3. `removeUnusedFunctions` (raíces: `main`, linkage externo, handlers, funciones con dirección tomada
   por datos).

Inlining actual: **solo callee de un bloque, sin llamadas, ≤32 instrucciones (≤160 con `inline`), que
termina en `Return`**. No inlinea variádicas, ni handlers, ni `main`, ni recursión.

No-optimizaciones deliberadas: `Load` volátil nunca se elimina; división/módulo por cero constante
nunca se pliega.

### 8.3 Flags de optimización (`optimization.h`)

19 flags, todos ON en `-O1`, todos OFF en `-O0`: `const-fold`, `algebraic`, `branch-simplify`, `dce`,
`unreachable-blocks`, `jump-threading`, `inline` (O2), `local-slot-reuse`, `copy-propagation`,
`load-forwarding`, `dead-stores`, `unused-functions`, `jump-tables`, `frameless-leaf`, `regalloc`,
`cmp-branch-fusion`, `immediates`, `fallthrough`, `address-folding`. `-O3` es alias de `-O2`.

### 8.4 Asignación de registros (`value_placement.cpp`)

- **Liveness** hacia atrás sobre el CFG hasta punto fijo.
- **Escape** de locales (dirección solo como operando de `Load`/`Store`).
- **Pools**: caller-saved entero `{6,7,12}` / `{6,7,12,0,1,2,3}` hoja; flotante `{6,7}` /
  `{6,7,0,1,2,3}` hoja; **callee-saved entero `{8,9,10,11}` y flotante `{8..15}` usados cuando la
  función llama** (con `pushm`/`popm`/`fpushm`/`fpopm`); scratch fijos `r4`/`r5`, `f4`/`f5`;
  `r13`/`at` **nunca** asignado.
- Locales primero (toda la función), dos pasadas (`register` antes). Temporales cross-bloque reservan
  registro por bloque; luego escaneo lineal por bloque. Espills comparten slot por interferencia.
- **Frameless leaf**: sin `enter`/`leave` si no se necesita frame.

> **Contrato con `asm/setjmp.casm` (crítico).** `setjmp` guarda `r8-r11`, `f8-f15`, `fp`, `sp` y el PC
> **asumiendo exactamente el conjunto callee-saved de esta ABI**. Cualquier cambio del asignador que
> altere qué registros sobreviven a una llamada (p. ej. empezar a usar otro registro como
> callee-saved, o dejar de preservar uno) rompe `longjmp` **en silencio**. Ver F4 y O4.

### 8.5 Frames y ABI

- El frame se emite como `struct` de CASM real (`struct __frame_<fn> slotN: u32`).
- `enter`/`leave`; slots fuera de ±32 KiB con `at`; frames >64 KiB con `enter` vacío + `sub sp, sp, at`.
- Llamada: 4 args enteros `r0`–`r3`, 4 flotantes `f0`–`f3` contados aparte, resto en el área saliente;
  retorno en `r0`/`f0`; indirecta materializa en `r12` y emite `call r12`.
- Varargs: el *tail* siempre por pila; `va_list` es `char*`. **`printf`/`scanf` de la STDLIB se apoyan
  literalmente en esta ABI** (`include/stdarg.h` mapea los builtins).
- Interrupciones: prólogo/epílogo propios (`pushm 0x1FFF`, `fpushm`, `iret`); el `at` no se preserva.
- `main`: apaga la máquina con status en bits 15:8; si el unit declara `void exit(int)`, lo llama. La
  STDLIB lo usa: `src/exit.c` dice literalmente que "ceresc turns `return n;` into `exit(n)`".

### 8.6 Peepholes de codegen

`cmpBranchFusion`, `immediateOperands`, `fallthroughBranches`, `addressFolding` y supresión de `Const`
tomados como inmediatos. Síntesis de `setcc` (no hay setcc): comparación usada como valor → 4
instrucciones.

---

## 9. Capacidades avanzadas ya presentes

| Capacidad | Detalle |
| --- | --- |
| **Switch a tabla/árbol** | Tabla `.rodata` (`LDRX`+`JPR`) si denso (≥5 casos, ≤256 entradas, densidad ≥0.5); árbol si disperso (≥8); cadena en `-O0` |
| **Varargs** | ABI propia; `va_list` = `char*`; sin `<stdarg.h>` real (builtins) |
| **Interrupciones** | `__interrupt` + `__interrupt_vector`; guardado íntegro; `sti`/`cli`/`halt` |
| **Interop C↔CASM** | Símbolos sin decorar; reservadas → `__c_`; `__asm__("label")`; ficheros de declaraciones |
| **Compilación separada** | `.cobj`/`.car` sin recompilar; `--decls`/`--emit-decls` |
| **Callee-saved** | Locales a través de llamadas con `pushm`/`popm` |
| **Escape + liveness** | Análisis real sobre el CFG |
| **`volatile`/`restrict`** | Cargas volátiles no se eliminan; forwarding por `restrict` |
| **Frameless leaf** | Sin `enter`/`leave` si no se necesita frame |
| **Extensiones modernas** | `_Generic`, literales compuestos, designadores, `__attribute__`, `_Static_assert` |
| **Ensamble en línea** | `__asm__("...")` tratado como llamada para register allocation |

---

## 10. La biblioteca estándar Ceres STDLIB

Esta sección es nueva y central: es el "usuario número uno" del compilador y la fuente de varios
requisitos.

### 10.1 Composición

| Parte | Contenido |
| --- | --- |
| `include/` (88 `.h`) | Cabeceras estándar C89/C99 y cabeceras de plataforma |
| `include/ceres/` (~40 `.h`) | Dispositivos y servicios de Ceres: `terminal`, `timer`, `disk`, `display`, `gfx`, `font`, `tui`, `audio`, `keyboard`, `mouse`, `gamepad`, `input`, `key`, `line`, `fs`, `heap`, `pool`, `arena`, `rand`, `hash`, `bits`, `fixed`, `ns64`, `sys`, `irq`, `atomic`, `debug`, `color`, `sprite`, `textfb`, `vecmath`, `periph`, `dma`, `game`, `test`, `config`, `string_fast` |
| `include/ceres/ds/` (~25 `.h`) | Contenedores: `vector`, `list`, `slist`, `deque`, `stack`, `cqueue`, `ringbuf`, `hashmap`, `hset`, `gmap`, `omap`, `multimap`, `flatmap`, `flatset`, `oset`, `bitset`, `bloom`, `rbtree`, `skiplist`, `trie`, `iheap`, `pqueue`, `lru`, `slotmap`, `strbuf`, `dsu`, `generic` |
| `src/` (74 `.c`, ~10 000 líneas) | Implementaciones, incluidas `malloc`, `format` (printf), `scanf`, `file`/`fopen`, `fs` (CeresFS, 727 líneas), `math` (429) + `math_ext` (317), `strtox`, `time`, `signal`, `ctype`, `locale`, `errno`, `stdio`, `string`, `stdlib` |
| `asm/` (8 `.casm`) | `bits`, `math_ops`, `memory`, `string_fast`, `sys`, `setjmp`, `optional/fault`, `optional/game_wait` |
| `tools/` | `mklib.ps1` (construye el archivo), `runtests.ps1` (74 tests × 3 niveles), `common.ps1`, generadores JS de tablas/fuentes |
| `tests/` (74) y `examples/` (15) | Pruebas byte-a-byte y programas reales (`pong`, `snake`, `life`, `mandelbrot`, `calc`, `guess`, `tune`, `timerdemo`, `fsdemo`, …) |

### 10.2 Cabeceras estándar cubiertas

`assert.h`, `ctype.h`, `errno.h`, `float.h`, `inttypes.h`, `iso646.h`, `limits.h`, `locale.h`,
`math.h`, `setjmp.h`, `signal.h`, `stdarg.h`, `stdbool.h`, `stddef.h`, `stdint.h`, `stdio.h`,
`stdlib.h`, `string.h`, `strings.h`, `time.h`, `ceres.h` e `interrupts.h`.

Lo que ofrecen es C real adaptado a las limitaciones, y lo dicen:
- `<stdint.h>` **omite todo lo de 64 bits**, por honestidad.
- `<limits.h>`/`<float.h>`/`<stdlib.h>` documentan que `long`/`long long` son `int` y `double` es `float`.
- `<math.h>` publica las funciones de una instrucción (`fabs`, `fmod`, `sqrt`, `floor`, `ceil`, `trunc`,
  `fmin`, `fmax`, `copysign`, `fma`, `rcp`, `rsqrt`, `fpclassify`, `isnan`, `isinf`, `signbit`, …) con
  implementación en CASM, y las de software (`sin`, `cos`, `pow`, `log`, `exp`, …) en C con precisión
  declarada (~2e-7 relativo; `pow` con exponente grande ~2e-6).
- `<stdio.h>` tiene `printf`/`scanf` completos (`%d %i %u %o %x %X %b %c %s %p %n %f %F %e %E %g %G`,
  flags, ancho, precisión, longitud), y `FILE` sobre terminal, memoria y **CeresFS** en disco.
- `<setjmp.h>` da `setjmp`/`longjmp` reales.
- `<signal.h>` da `SIGABRT`/`SIGINT`/`SIGTERM`/`SIGFPE`/`SIGILL`/`SIGSEGV`, apoyándose en el módulo
  opcional de fallos.

### 10.3 Cómo la STDLIB depende del compilador (y lo que más le duele)

| Servicio de la STDLIB | Depende de |
| --- | --- |
| `printf`/`scanf` | Varargs, `__builtin_va_list`, `switch`, punteros, `union` (bits de float), `float` |
| `malloc`/`free` | Estructuras y punteros, `sys_sp()`/`sys_heap_start()` (CASM), aritmética de punteros |
| `setjmp`/`longjmp` | El **conjunto callee-saved** de la ABI, fp/sp/pc, flags implícitos |
| `signal`/fault | `__interrupt`, `__interrupt_vector`, `__builtin_sti/cli/halt`, `longjmp` |
| `math_ops`/`bits` | Instrucciones que C **no puede deletrear** (`fclass`, `mff`/`mtf`, `sqrt`, `fma`, `clz`, `popcnt`, `bswap`, `rol`) |
| `atomic.h` | `irq_save`/`irq_restore` (CASM: `push`/`pop` para leer flags), `volatile` |
| `ns64`, `time`, `fx_mul` | Emulación software de 64 bits (no hay `int64_t`) |
| `fs`/`file`/`fopen` | `malloc`, memoria, DS, estructuras, `switch` |

### 10.4 Los ficheros CASM de la STDLIB: el mapa de carencias del compilador

- `asm/bits.casm`: `bit_clz`, `bit_ctz`, `bit_popcount`, `bit_bswap`, `bit_rotl`, `bit_rotr`,
  `umulhi`, `imulhi`, `fx_mul`. Comentario de `include/ceres/bits.h`: *"The bit instructions C has no
  way to spell"*.
- `asm/math_ops.casm`: `fabs`, `fmod`, `sqrt`, `floor`, `ceil`, `rint`/`nearbyint` (`fround`), `trunc`,
  `fmin`, `fmax`, `copysign`, `fma`, `rcp`, `rsqrt`, `isnan`, `isinf`, `signbit`, `fpclassify`,
  `float_bits`, `float_from_bits` (`mff`/`mtf`).
- `asm/sys.casm`: `sys_sp` (`mov r0, sp`), `irq_save` (`push`/`pop` para leer los flags), `irq_wait`
  (`sti`+`halt` atómicos), `sys_heap_start` y `sys_get_layout` (símbolos del linker).
- `asm/setjmp.casm`: `setjmp`/`longjmp` codificando a mano el conjunto callee-saved.
- `asm/memory.casm` y `asm/string_fast.casm`: `memcpy`/`memmove`/`memset`/`memcmp`/`strlen` y
  `strcpy_fast`/`strcmp_fast`/`strchr_fast`/`memchr_fast` **palabra a palabra**, con los punteros
  alineados y la prueba `(w - 0x01010101) & ~w & 0x80808080`.
- `asm/optional/fault.casm`: bindea los vectores de excepción y llama al reportero en C.

**Estos ficheros son la prueba directa de los vacíos del compilador**: su razón de existir es que C en
Ceres-C no alcanza ciertas instrucciones ni ciertos registros/estados. Las propuestas F1, F2, F4, F10 y
F14 atacan exactamente eso.

### 10.5 La medición de rendimiento propia de la STDLIB

`asm/string_fast.casm` documenta (en instrucciones, contadas por el Timer, idénticas en cada ejecución;
sobre 4096 bytes, **contra las versiones C a `-O2`**):

| Rutina | ASM a mano | C a `-O2` | Ratio |
| --- | --- | --- | --- |
| `strcpy` | 11 305 | 32 797 | 2.9× |
| `strcmp` | 13 356 | 40 995 | 3.1× |
| `strchr` | 15 407 | 45 089 | 2.9× |
| `memchr` | 13 347 | 36 896 | 2.8× |

Y añade: a `-O0` las versiones C tardan **diez veces** lo que a `-O2`. Este dato es el que motiva O15
(reconocimiento de idiomas de bucle byte→palabra) y refuerza O1/O3/O12: el compilador genera código
correcto pero deja sobre la mesa un factor ~3 en los bucles que dominan una biblioteca C.

### 10.6 Configuración y filosofía

`include/ceres/config.h` permite compilar la biblioteca con opciones por build (`-DCERES_ATEXIT_SLOTS=8`,
`-DFS_MAX_OPEN=4`, `-DTIMER_MAX_TASKS=…`), y `tests/expected/<name>.flags` prueba configuración
alternativa. La biblioteca es **didáctica y explícita**, en la misma línea que el compilador y la VM:
nada de magia oculta, cada límite documentado, errores con `errno`, y la frontera con el hardware
expuesta en `ceres/*.h`.

---

## 11. Lo que el compilador NO usa de la máquina (y cómo lo suple la STDLIB)

| Instrucción / recurso | Estado en Ceres-C | Cómo lo suple la STDLIB |
| --- | --- | --- |
| `ADDC`/`SUBC`/`ADDCI`/`SUBCI` | **Sin usar** (no hay 64 bits reales) | Emulación de 64 bits en C (`ns64.c`, `time.c`) |
| `MULH`/`IMULH` | **Sin usar** | `asm/bits.casm` (`umulhi`, `imulhi`, `fx_mul`) |
| `ABS`, `MIN`/`IMIN`/`MAX`/`IMAX` | Solo `min rd,rs,1` para `ToBool` | — |
| `CLZ`/`CTZ`/`POPCNT`/`BSWAP`/`ROL`/`ROR` | **Sin usar** | `asm/bits.casm` |
| `FSQRT`/`FABS`/`FMA`/`FCLASS`/`FRECIPE`/`FRSQRTE` | **Sin usar** | `asm/math_ops.casm` |
| `FMOD`/`FMIN`/`FMAX`/`FROUND`/`FFLOOR`/`FCEIL`/`FTRUNC`/`FCOPYSIGN` | **Sin usar** | `asm/math_ops.casm` |
| `MTF`/`MFF` (bits float↔int) | **Sin usar** | `asm/math_ops.casm` + `union` en `src/format.c` |
| `JO`/`JNO` (overflow) | **Sin usar** | Sin uso (no hay `<stdckdint.h>`) |
| `BL`/`BLR` | **Sin usar** | `asm/memory.casm` usa tail-jump a `memcpy` |
| `LDRP`/`STRP` (PC-relativo) | **Sin usar** | El ensamblador relaja `ldv`/`stv` cuando puede |
| `TRAP`, `INT imm8` | **Sin usar** desde C | `signal()` usa el módulo de fallos |
| MMU (`MTP`/`PGON`/...) | **Sin usar** | Sin uso (no hay API de paging) |
| Leer `sp`/flags; `push`/`pop` | **Sin usar** (inalcanzable en C) | `asm/sys.casm` (`sys_sp`, `irq_save`) |
| Debug info (líneas/símbolos) | **Sin usar** | El reportero de fallos solo imprime PC crudo |

---

## 12. Propuestas de nuevas características (F1–F14)

Cada propuesta indica **qué**, **evidencia**, **encaje**, **esfuerzo** (S/M/L), **impacto** y, cuando
aplica, **beneficio concreto para la STDLIB**.

### F1 — Builtins nativos para las instrucciones de la ISA (bits y flotante de una instrucción)

- **Qué**: `__builtin_clz/ctz/popcount/bswap/rotl/rotr/mulh` y `__builtin_fabs/fmod/sqrt/floor/ceil/
  trunc/rint/fmin/fmax/copysign/fma/rcp/rsqrt/fclass/float_bits/float_from_bits`.
- **Evidencia**: `include/ceres/bits.h` (*"las instrucciones de bits que C no puede deletrear"*),
  `include/math.h` (las funciones de una instrucción documentadas como `asm/math_ops.casm`),
  `asm/bits.casm`, `asm/math_ops.casm`; ninguna de estas instrucciones se emite desde C.
- **Encaje**: parser (reconocer `__builtin_x(`), `ast/expr.h`, sema (aridad/tipo), IR (`IrUnOp` o nuevo
  opcode; `IrMachineOp` para las que no tienen resultado), codegen, docs, tests.
- **Beneficio STDLIB**: `asm/bits.casm` y la mayor parte de `asm/math_ops.casm` dejan de ser necesarios;
  al ser código de compilador, **el plegado de constantes y el CSE los alcanzan** y no exigen un
  `.casm`+`--decls` por función; el usuario los obtiene sin enlazar `libceres`.
- **Esfuerzo**: M. **Impacto**: alto.

### F2 — Aritmética marcada y producto alto

- **Qué**: `__builtin_add_overflow`/`sub_overflow`/`mul_overflow`, `__builtin_mulh/mulhu`.
- **Evidencia**: `JO`/`JNO`, `MULH`/`IMULH` existen y no se usan; `asm/bits.casm` ya expone
  `umulhi`/`imulhi` a mano.
- **Encaje**: operación IR que lea Overflow tras la aritmética; sema con puntero de salida; codegen con
  `JO/JNO`.
- **Beneficio STDLIB**: `alloc`/`realloc`/`calloc` (`malloc.c` ya comprueba el desbordamiento de `n*size`
  a mano), `ns64`, `fixed` y `rand` ganan precisión y seguridad.
- **Esfuerzo**: M. **Impacto**: medio-alto.

### F3 — Enteros de 64 bits reales (`long long` / `int64_t`)

- **Qué**: soporte real de 64 bits (par de registros), eliminando W2001.
- **Evidencia**: `ADDC`/`SUBC` forman cadenas de acarreo y `MULH`/`IMULH` dan el producto alto;
  `include/stdint.h` **omite deliberadamente** `int64_t`; `src/ceres/ns64.c` (157 líneas) y `src/time.c`
  reimplementan 64 bits a mano.
- **Encaje**: `ast/type.h` (tipo de 8 bytes), layout, sema (promociones), IR (legalización a 2 palabras),
  codegen (rutinas `__cc_add64`/`__cc_mul64`/`__cc_div64`), ABI para args/retorno de 64 bits.
- **Beneficio STDLIB**: `<stdint.h>` puede definir `int64_t`/`uint64_t`; `ns64`/`time` se simplifican;
  `strtoll`/`lldiv` dejan de ser alias del `int`.
- **Esfuerzo**: L. **Impacto**: medio-alto.

### F4 — `setjmp`/`longjmp` nativos (o contrato de ABI verificado)

- **Qué**: que el compilador conozca `setjmp`/`longjmp` (`returns_twice`), o bien un builtin
  `__builtin_setjmp`/`__builtin_longjmp`, o como mínimo un **contrato de ABI versionado y testeado**.
- **Evidencia**: `asm/setjmp.casm` codifica a mano `r8-r11`, `f8-f15`, `fp`, `sp`, `pc`, y su comentario
  lo dice: *"the compiler keeps locals in r8-r11"*. Es un acoplamiento frágil con el asignador.
- **Encaje**: builtins en parser/sema/IR/codegen; o `__attribute__((returns_twice))` + validación; test
  de ABI obligatorio en el pipeline.
- **Beneficio STDLIB**: `signal`/fault (`src/signal.c`, `src/ceres/fault.c`) dependen de `longjmp` para
  reanudar tras un fallo; un cambio de RA no debe romperlos en silencio.
- **Esfuerzo**: M. **Impacto**: medio-alto (corrección latente).

### F5 — Integración de primera clase de la STDLIB en el driver

- **Qué**: `--sysroot <dir>` / `-L <dir> -lceres` / búsqueda de `#include <ceres/...>` y
  `__has_include`; verificación de que `libceres` y el compilador comparten ABI/versión.
- **Evidencia**: hoy un build es `ceresc prog.c build/lib/O2/libceres.car --decls … -I include -O2 --run`
  y el descubrimiento de rutas vive en PowerShell (`tools/common.ps1`); no hay forma de `#include
  <stdio.h>` sin `-I include`.
- **Encaje**: `libs/driver` (nuevas opciones), `ceres_locator` ampliado, `__has_include` en el
  preprocesador.
- **Beneficio STDLIB**: DX mucho mejor; deja de depender de scripts de ayuda para usar la biblioteca.
- **Esfuerzo**: S–M. **Impacto**: alto.

### F6 — `alloca`, *flexible array members* y VLA

- **Qué**: `alloca(n)`/`__builtin_alloca`, `struct S { int n; int a[]; }`, VLAs de bloque.
- **Evidencia**: arrays de tamaño fijo; la STDLIB tiene arenas en C (`ceres/arena.h`, `ceres/pool.h`) y
  buffers fijos por todas partes; `scanf`/`format` y parsers se beneficiarían de buffers temporales.
- **Encaje**: parser, sema, IR/codegen (ajuste de `sp` y `FrameAddr` calculado).
- **Esfuerzo**: M. **Impacto**: medio.

### F7 — Bitfields y layout empaquetado

- **Qué**: bitfields según C y `#pragma pack`/`__attribute__((packed))` con acceso *load-modify-store*.
- **Evidencia**: `packed` es **error E2043**; bitfields fuera. La STDLIB empaqueta bits a mano
  (`include/ceres/font_data.inc`, flags en `src/ceres/fs.c`, `src/ceres/input.c`).
- **Encaje**: `sema/type_layout.h`, `ir_builder.cpp` (máscara/shift), tests de layout.
- **Esfuerzo**: M–L. **Impacto**: medio.

### F8 — Información de depuración de C (`ceresc --debug`)

- **Qué**: que `ceres debug` muestre **líneas y símbolos de C**. Opciones: (a) directivas de línea que el
  ensamblador registre; (b) sidecar CASM→(fichero, línea) y tipos C; (c) sintetizar el bloque CDBG.
- **Evidencia**: CeresASM ya tiene tabla de líneas + símbolos y depurador (docs/21, docs/22); Ceres-C
  solo añade comentarios que el ensamblador no lee; `include/ceres/sys.h` y `src/ceres/fault.c` ya
  anticipan el beneficio ("with --debug the address can be turned into file:line").
- **Encaje**: `CasmEmitter` + driver.
- **Beneficio STDLIB**: los fallos reportados por el módulo de fallos y los breakpoints del depurador
  señalarían líneas de C de la biblioteca (hoy apuntan al `.casm` generado).
- **Esfuerzo**: L. **Impacto**: muy alto.

### F9 — `case` con rangos (`case low ... high:`) y `switch` en tipos anchos

- **Qué**: extensión GNU de rangos y `switch` sobre `long long` (si F3 existe).
- **Evidencia**: la STDLIB usa `switch` intensivamente (`irq_name`, `errno`, `key`, `time`, `hash`,
  `color`, `fixed`); la tabla/árbol ya están implementados y rellenar un rango es trivial.
- **Encaje**: parser, sema, `ir_builder.cpp` (`collectSwitchCases`).
- **Esfuerzo**: S. **Impacto**: medio.

### F10 — Builtins de máquina y del compilador

- **Qué**: `__builtin_trap()` (`TRAP`), `__builtin_int(n)` (`INT imm8`), `__builtin_unreachable()`,
  `__builtin_expect()`, `__builtin_constant_p()`, `__builtin_offsetof(T,m)`.
- **Evidencia**: `TRAP`/`INT` existen y **no se pueden invocar desde C** (docs/10: "No `int n` from C").
  `offsetof` hoy es la macro `((size_t)&(((T*)0)->m))` (`include/stddef.h`), que un builtin hace más
  segura.
- **Encaje**: parser, sema, IR (`IrMachineOpPayload` ya existe), codegen.
- **Beneficio STDLIB**: `expect`/`unreachable` guían al optimizador en `printf`, `fs`, `key`; `trap` para
  `abort()`; `offsetof` robusto para las DS.
- **Esfuerzo**: S. **Impacto**: medio.

### F11 — Atributos con efecto real

- **Qué**: semántica a `noreturn`, `pure`, `const`, `noinline`, `always_inline`, `weak`, `alias`,
  `section`, `deprecated`, `warn_unused_result`, `nonnull`, `format(printf,…)`.
- **Evidencia**: docs/02 dice que casi todos se **aceptan y descartan en silencio**. La STDLIB resuelve
  "módulos opcionales" con objetos sueltos + punteros a función (`irq_attach`, `__abort_hook`,
  `__signal_hw_ready`); `weak`/`alias` darían overrides limpios. `format(printf,...)` detectaría errores
  en las llamadas a `printf` de la propia biblioteca y del usuario.
- **Encaje**: sema (almacenar), `ir_optimizer.cpp` (Call pura), codegen (`section`/`weak`/`alias`).
- **Esfuerzo**: M. **Impacto**: medio-alto.

### F12 — Preprocesador completo (`#include_next`, `_Pragma`, `__has_include`, `#line`, `#embed`)

- **Qué**: las directivas que faltan.
- **Evidencia**: `__has_include` es clave para `ceres/config.h` y detección de plataforma; `#embed`
  encaja con `font_data.inc`/tablas generadas; `#line` con los generadores (`tools/gen_*.js`).
- **Encaje**: `preprocessor.cpp` + `line_map`.
- **Esfuerzo**: S–M. **Impacto**: medio.

### F13 — LTO / IR de programa completo

- **Qué**: recopilar el IR de todas las unidades (incluidas las de `libceres.car`) y optimizar antes del
  codegen, permitiendo inlining intermodular.
- **Evidencia**: la STDLIB es un archivo de ~70 objetos y hoy el inlining es **intra-unidad**; `putchar`,
  `fgetc`, `memcpy`, `math_ops`, `qsort` no se inlinean jamás.
- **Encaje**: `driver.cpp` (fase IR común), `ir_optimizer.cpp` (alcance multi-módulo), o `.cir`
  (IR serializado) por unidad.
- **Esfuerzo**: L. **Impacto**: alto.

### F14 — `__asm__` con operandos/clobbers, o builtins de estado de máquina

- **Qué**: (a) `__asm__("..." : outputs : inputs : clobbers)` (hoy E2044), o (b) builtins
  `__builtin_stack_pointer()`, `__builtin_frame_address()`, `__builtin_flags()`,
  `__builtin_irq_save()`/`__builtin_irq_restore()`.
- **Evidencia**: `asm/sys.casm` hace `mov r0, sp`, `push`/`pop` para leer los flags y `sti`+`halt`
  atómicos; `asm/setjmp.casm` necesita sp/fp/pc; C no puede leer `sp` ni los flags ni hacer `push`/`pop`.
- **Encaje**: parser + IR + codegen; los builtins de flags son el caso más limpio.
- **Beneficio STDLIB**: `irq_save`/`irq_restore`/`sys_sp`/`irq_wait` podrían ser C; menos CASM que
  mantener.
- **Esfuerzo**: M–L (a), S–M (b). **Impacto**: medio.

---

## 13. Propuestas de mejoras y optimizaciones (O1–O15)

### O1 — Reducción de fuerza para multiplicación/división/módulo por constante

- **Qué**: `x * 2^k` → `shl`; `x / 2^k` y `x % 2^k` (con signo) → shift + corrección; `x / C` general →
  inverso mágico con `MULH`/`IMULH`.
- **Evidencia**: `codegen.cpp:739-741` emite siempre `imul`/`idiv`/`imod`; no hay reducción de fuerza.
  `MULH`/`IMULH` y `SHLI`/`SARI` sin usar (la STDLIB los usa a mano en `bits.casm`).
- **Beneficio STDLIB**: `itoa`/`format` (bases 10/16), `strtox`, `ns64_div_u32`, `fx_div`, hashing.
- **Encaje**: nuevo pase IR (`strengthReduction`) o peephole en codegen.
- **Esfuerzo**: M. **Impacto**: alto.

### O2 — Eliminación de subexpresiones comunes (CSE/GVN)

- **Qué**: reutilizar resultados ya calculados con los mismos operandos, intra y (cuando el CFG lo
  permita) inter-bloque.
- **Evidencia**: `ir_optimizer.cpp` **no tiene ningún pase de CSE/GVN**.
- **Beneficio STDLIB**: `format`/`scanf`/`fs`/`gfx` repiten cálculos (máscaras, anchos, direcciones).
- **Encaje**: nuevo pase con numeración de valores (volátiles, stores y llamadas como barreras).
- **Esfuerzo**: M–L. **Impacto**: alto.

### O3 — Optimizaciones de bucle

- **Qué**: LICM, reducción de fuerza de variables de inducción, rotación y desenrollado.
- **Evidencia**: el optimizador **no tiene noción de bucle**. La STDLIB tiene bucles por todas partes
  (`memcpy`/`strlen` C, `format`, `fs`, `math`, `gfx`).
- **Encaje**: detección de bucles naturales sobre el CFG; LICM e IV-SR.
- **Esfuerzo**: L. **Impacto**: alto.

### O4 — Asignador de registros de mejor calidad

- **Qué**: linear scan/coloring con coalescing, splitting de rangos, coste de spill, `at` (r13) bajo
  bandera y asignación por presión real (hoy "función con calls veta `r0`–`r3` entera").
- **Evidencia**: `value_placement.cpp` es correcto pero conservador; el propio docs/12 Fase 3/Opción D
  propone el refinamiento.
- **Riesgo/contrato**: **cualquier cambio debe preservar el conjunto callee-saved que `setjmp.casm`
  asume**; añadir un test de ABI es requisito, no opcional (ver F4).
- **Esfuerzo**: L. **Impacto**: alto.

### O5 — Eliminación de copias y *move coalescing*

- **Qué**: eliminar `mov rX,rY` (incluido `mov rX,rX`), propagar la copia al consumidor y asignar al
  destino el registro de la fuente.
- **Evidencia**: el CASM optimizado del tutorial (`docs/04`) tiene cadenas de `mov` alrededor de cada
  uso; en bibliotecas grandes eso se multiplica.
- **Encaje**: peephole de codegen + coalescing en `ValuePlacement`.
- **Esfuerzo**: M. **Impacto**: medio-alto.

### O6 — `setcc` de una instrucción con cadenas de acarreo

- **Qué**: para comparaciones usadas como valor, `ADDC rd, rz, rz`/`SUBC` en vez de 4 instrucciones;
  firmadas con signo/overflow.
- **Evidencia**: docs/03 documenta la ausencia de `setcc` y su síntesis de 4 instrucciones; `ADDC`/`SUBC`
  sin usar.
- **Encaje**: `codegen.cpp` `materializeCmp`/`cmpBranchFusion`.
- **Esfuerzo**: M. **Impacto**: medio.

### O7 — Optimización de llamada de cola y `BL`/`BLR` en hojas

- **Qué**: `call f`+`ret` → salto; hojas con `BL`/`BLR` (sin tráfico de pila).
- **Evidencia**: `opcodes.h:151-156` documenta `BL`/`BLR` para exactamente esto; Ceres-C nunca los emite.
  `asm/memory.casm` ya practica el tail-jump a mano.
- **Beneficio STDLIB**: `math_ops`/`bits` (hojas puras), `memmove`→`memcpy`, dispatchers.
- **Esfuerzo**: M–L. **Impacto**: medio-alto.

### O8 — Acceso PC-relativo a estáticos (`LDRP`/`STRP`) y mejor direccionamiento

- **Qué**: accesos PC-relativos de una palabra para símbolos cercanos (±32 KiB) en vez de `la` (2
  palabras) + `ldr/str`.
- **Evidencia**: `LDRP`/`STRP` sin usar; victoria de tamaño en `.text`.
- **Beneficio STDLIB**: multitud de globales (`stdout`, `errno`, tablas, `irq_table`).
- **Encaje**: codegen con información de sección; o pseudo que el ensamblador relaje.
- **Esfuerzo**: M. **Impacto**: medio.

### O9 — Reordenación de bloques para maximizar el fall-through

- **Qué**: layout que haga caer el sucesor más probable, agrupe código caliente y aleje el frío.
- **Evidencia**: hoy solo existe `fallthroughBranches`, que **elimina** saltos a bloques adyacentes,
  pero **no hay pase que reordene** para crearlos.
- **Beneficio STDLIB**: biblioteca cargada de `switch` (`irq_name`, `errno`, `key`, `color`, `fixed`).
- **Esfuerzo**: M. **Impacto**: medio.

### O10 — Mejoras de inlining

- **Qué**: inline multi-bloque, de funciones que llaman (remapeando `BasicBlock*`), con modelo de coste,
  *partial inlining* y `always_inline`.
- **Evidencia**: `isInlinable()` exige un bloque, sin llamadas, ≤32 instrucciones.
- **Beneficio STDLIB**: funciones diminutas frecuentes (`putchar`, `getc`, `fputc`, wrappers de
  `math_ops`) hoy nunca se inlinean; **combinado con F13** es donde más se gana.
- **Esfuerzo**: L. **Impacto**: alto.

### O11 — Propagación de constantes condicional (SCCP) y poda de ramas

- **Qué**: propagar constantes según aristas tomadas/no tomadas, podar ramas/bloques y resolver
  `switch` constante.
- **Evidencia**: `foldFunction` propaga constantes pero **no es consciente de la rama tomada**.
- **Beneficio STDLIB**: `ceres/config.h` y los `switch` de clasificación se reducen cuando el valor es
  constante.
- **Esfuerzo**: M–L. **Impacto**: medio-alto.

### O12 — Selección de patrones con la ISA extendida (min/max/abs/clz/ctz)

- **Qué**: `x<0 ? -x : x` → `ABS`; `a>b?a:b` → `IMAX`/`MAX`; `x & -x`/potencias → `CTZ`/`CLZ`; producto
  alto.
- **Evidencia**: `ABS`/`MIN`/`MAX` sin usar; `fx_abs` en `include/ceres/fixed.h` es literalmente
  `a < 0 ? -a : a`; `bit_is_pow2`/`bit_next_pow2`/`bit_log2` en `bits.h` son candidatos.
- **Esfuerzo**: M. **Impacto**: medio-alto (con el matiz de que `bits.casm` ya los usa; aquí se trata de
  que el compilador los *reconozca y genere* sin la biblioteca).

### O13 — Niveles `-Os`/`-Og`, LTO y estadísticas de pases

- **Qué**: `-Os` (tamaño), `-Og` (depuración), `-flto` (F13) y `-fstats`/`-ftime-report`.
- **Evidencia**: hoy solo `-O0/-O1/-O2` (y `-O3` alias); la única observabilidad del back-end es
  `--emit-ir`.
- **Beneficio STDLIB**: permitiría construir `libceres` optimizada para tamaño en builds de juego, y
  medir el efecto de cada optimización.
- **Esfuerzo**: S–M. **Impacto**: medio.

### O14 — DSE y forwarding conscientes de llamadas/alias

- **Qué**: extender `eliminateDeadStores`/`forwardLoads` (hoy *whole-local* y por bloque) a
  reaching-stores con alias/escape.
- **Evidencia**: `eliminateDeadStores` (`ir_optimizer.cpp:945`) es explícitamente whole-local;
  `forwardLoads` no hace merge entre bloques.
- **Esfuerzo**: L. **Impacto**: medio.

### O15 — Reconocimiento de idiomas de bucle byte→palabra (idiom recognition)

- **Qué**: reconocer los bucles idiomáticos de copia/relleno/búsqueda byte a byte
  (`for (i=0;i<n;i++) d[i]=s[i];`, `while (*s) s++;`, `memset`-like) y bajarlos a:
  (a) operaciones palabra-a-palabra alineadas, o (b) llamadas a las rutinas ya optimizadas de la
  biblioteca (`memcpy`/`memset`/`strlen`/`strcmp`/`memchr`), o (c) una expansión inline de las mismas.
- **Evidencia**: es **la carencia más medida del compilador**. `asm/string_fast.casm` demuestra que el
  código C a `-O2` es ~3× más lento que el equivalente a mano para `strcpy`/`strcmp`/`strchr`/`memchr`.
  El compilador no reconoce el patrón ni aprovecha la alineación ni los cuatro bytes por iteración.
- **Beneficio STDLIB**: `src/string.c` y `src/memory` (versiones C) dejarían de ser el eslabón lento;
  potencialmente se podría prescindir de parte de `asm/string_fast.casm`.
- **Encaje**: pase IR de reconocimiento de bucles (apoyado en la detección de bucles de O3) + expansión
  en codegen.
- **Esfuerzo**: L. **Impacto**: alto (rendimiento de biblioteca y de programas reales).

---

## 14. Desincronización documentación ↔ código

Hallazgos (útiles como *issues* de mantenimiento):

**Ceres-C**

1. **`docs/12-Register-Allocation-Extension-Plan.md`** dice "propuesta de diseño, no aplicada" y describe
   `r8-r11`/`f8-f15` como "nunca usados". El código **ya los implementa** (`value_placement.cpp:40-48,
   339-373`; `codegen.h:357`; `pushm`/`popm`/`fpushm`/`fpopm`). Debe marcarse como **implementado
   (Fase 1)**.
2. **`docs/01-Getting-Started.md:106`** afirma "there is no `#ifndef` in this version", en contradicción
   con `README.md:90`, `docs/02-Grammar.md:91` y `docs/08-Preprocessor.md:15`, que lo documentan como
   implementado. El ejemplo de error de `docs/08:32` también está obsoleto.
3. **`docs/06-Known-Limitations.md`** dice que "no hay biblioteca estándar" y que "el `stdlib/` de
   CeresASM solo tiene `call.casm`", lo que **ignora el proyecto Ceres STDLIB** (§10). Corrección de la
   revisión 1 de este informe: la biblioteca existe; lo que no existe es que el compilador la conozca.
4. **`docs/13-Switch-Jump-Table-Plan.md`** está correctamente marcado como implementado y es coherente
   con `ir_builder.cpp:1733-1736`.

**Ceres STDLIB** (drift menor, en comentarios)

5. `include/ceres/atomic.h:42`, `include/ceres/test.h:12`, `include/ceres/ds/list.h:84` dicen que "the C
   subset has no comma operator", pero `docs/02-Grammar.md` y `src/string.c:5` confirman que **sí lo
   tiene** ahora.
6. `src/string.c:3-5` habla de "older ceresc builds, which lost the const of `const void*`", un fallo ya
   corregido en Ceres-C; el comentario y los casts sobrantes pueden retirarse.

Recomendación: un *documentation conformance test* en CI, y una prueba de ABI que falle si el conjunto
callee-saved cambia (por `setjmp`).

---

## 15. Matriz de esfuerzo/impacto y roadmap sugerido

| # | Propuesta | Tipo | Esf. | Impacto | Prioridad |
| --- | --- | --- | --- | --- | --- |
| F1 | Builtins de ISA (bits/flotante) | Feature | M | Alto | 1 |
| F5 | Integración STDLIB en el driver | Feature | S–M | Alto | 1 |
| F8 | Debug info de C | Feature | L | Muy alto | 1 |
| F11 | Atributos con efecto | Feature | M | Medio-alto | 1 |
| O15 | Idiomas de bucle byte→palabra | Optim | L | Alto | 1 |
| O1 | Reducción de fuerza const | Optim | M | Alto | 1 |
| O2 | CSE/GVN | Optim | M–L | Alto | 2 |
| O3 | Optimizaciones de bucle | Optim | L | Alto | 2 |
| O4 | Asignador de registros mejor | Optim | L | Alto | 2 |
| F13 | LTO / IR de programa completo | Feature | L | Alto | 2 |
| O5 | Coalescing / eliminación de copias | Optim | M | Medio-alto | 2 |
| F2 | Aritmética marcada / mulh | Feature | M | Medio-alto | 2 |
| F4 | setjmp/longjmp nativo + test ABI | Feature | M | Medio-alto | 2 |
| O8 | PC-relativo + direccionamiento | Optim | M | Medio | 2 |
| O13 | `-Os`/`-Og`/`-flto`/stats | Feature/Optim | S–M/L | Medio/Alto | 2–3 |
| O7 | Tail-call + BL/BLR | Optim | M–L | Medio-alto | 3 |
| O10 | Inlining mejorado | Optim | L | Alto | 3 |
| O11 | SCCP | Optim | M–L | Medio-alto | 3 |
| F3 | Enteros 64 bits | Feature | L | Medio-alto | 3 |
| F10 | Builtins de máquina | Feature | S | Medio | 3 |
| F9 | `case` con rangos | Feature | S | Medio | 3 |
| O6 | setcc de 1 instrucción | Optim | M | Medio | 3 |
| O9 | Layout de bloques | Optim | M | Medio | 3 |
| O12 | Selección min/max/abs/clz | Optim | M | Medio-alto | 3 |
| F6 | alloca / FAM / VLA | Feature | M | Medio | 4 |
| F7 | Bitfields / packed | Feature | M–L | Medio | 4 |
| F12 | Preprocesador completo | Feature | S–M | Medio | 4 |
| F14 | `__asm__` con operandos / builtins de sp/flags | Feature | M–L/S–M | Medio | 4 |
| O14 | DSE/forwarding con alias | Optim | L | Medio | 4 |

### Roadmap en tres olas

**Ola A — "cerrar vacíos y que la biblioteca vuele" (mes 1–2).**
`F1` (builtins de ISA) + `F5` (integración de la STDLIB) + `O15` (idiomas de bucle) + `O1` (reducción de
fuerza) + `F11` (atributos). Resultado: `asm/bits.casm` y `asm/math_ops.casm` dejan de ser necesarios,
la STDLIB se usa sin scripts, y los bucles de cadena/memoria se acercan al 3× de la asm a mano.

**Ola B — "calidad de código y observabilidad" (mes 2–4).**
`O2`, `O3`, `O4`, `O5`, `O8`, `F13`, `O13`, `F8` (debug info) y `F4` (setjmp + test de ABI). Resultado:
biblioteca más rápida y más pequeña, y depuración a nivel de C (fallos de la STDLIB citando líneas).

**Ola C — "profundidad de lenguaje" (mes 4+).**
`F2`, `F3`, `F6`, `F7`, `F9`, `F10`, `F12`, `F14`, `O6`, `O7`, `O9`, `O10`, `O11`, `O12`, `O14`.

### Criterio de aceptación transversal

Toda propuesta debe respetar la filosofía compartida por los tres proyectos:

- **mantener `-O0` funcionando y testeado**, con salida idéntica en `-O0/-O1/-O2` (los `examples` de
  Ceres-C y los 74 tests × 3 niveles de la STDLIB ya lo exigen);
- **opt-out individual** (`-f`/`-fno-`) y alta automática en la tabla de `optimization.h` (por lo que se
  apagan solos en `-O0`);
- **no romper el contrato de ABI con `asm/setjmp.casm`** sin actualizar y testear la biblioteca.

---

## 16. Reglas de ejecución de la Ola 1 (contrato de trabajo)

Esta sección fija el proceso con el que se implementa la **Ola A** del roadmap. Es un compromiso, no
una sugerencia, y se cumple mientras la ola esté en curso.

1. **Una parte = un commit.** La Ola 1 se divide en cinco partes: **O1** (reducción de fuerza), **F11**
   (atributos con efecto), **F1** (builtins de ISA), **F5** (integración de la STDLIB en el driver) y
   **O15** (reconocimiento de idiomas de bucle). Cada parte se implementa, se prueba y se commitea por
   separado.
2. **Cada commit se empuja.** Tras cada parte, `git push` a `origin/main`. El trabajo local no se
   acumula sin subir al terminar una parte.
3. **Verde antes de commitear.** Antes de cada commit: el árbol compila
   (`cmake --build --preset gcc-debug`) y `ctest --preset gcc-debug` pasa al 100 % con
   `CERESC_CERES_PATH` apuntando a un `ceres` construido, de modo que `e2e` y `examples` corran de
   verdad. Una parte no se commitea con tests rojos.
4. **Toda optimización es opt-out.** Cada optimización nueva entra en la tabla de
   `libs/support/include/ceresc/support/optimization.h` (nombre en `-f`/`-fno-`), queda ON en `-O1` y
   OFF en `-O0` por construcción, y los ejemplos deben imprimir lo mismo en `-O0`, `-O1` y `-O2`.
5. **Toda característica nueva lleva tests** en la suite de su fase (parser/sema/ir/codegen) y, cuando
   aplique, un caso en `examples/` con su `.expected`.
6. **No se rompe la ABI de la STDLIB.** Ningún cambio en el asignador puede alterar el conjunto
   callee-saved que asume `asm/setjmp.casm` (r8-r11, f8-f15); `--decls` e interop C↔CASM siguen
   valiendo.
7. **Pasada final con open code review.** Al completar las cinco partes, se ejecuta la revisión con
   `open-code-review` sobre el diff de la ola; los hallazgos se corrigen en un commit adicional y se
   documentan en esta sección.

### Estado de la Ola 1

| Parte | Estado | Commit |
| --- | --- | --- |
| **O1** — reducción de fuerza (`*2^k`, `/2^k`, `%2^k`) | **hecha** | `Add strength reduction for multiplication and division by a power of two` |
| **F11** — atributos con efecto (`noinline`, `always_inline`, `pure`, `const`, `deprecated`, `warn_unused_result`) | **hecha** | `Make __attribute__((...)) do something` |
| **F1** — builtins de una instrucción (`clz`/`ctz`/`popcount`/`bswap`/`rol`/`ror`/`mulh`/`abs` y el flotante de una instrucción) | **hecha** | `Add one-instruction machine builtins` |
| **F5** — integración de la STDLIB (`-L`/`-l`/`--sysroot`, `__has_include`) | **hecha** | `Add -L/-l/--sysroot and __has_include` |
| **O15** — reconocimiento de idiomas de bucle byte→palabra | **aplazada, con diseño** | — |

**O15 se aplaza a propósito.** Es la única parte que exige análisis de bucles naturales y una
reescritura palabra-a-palabra, y no puede entregarse sin arriesgar miscompilaciones entre `-O0`,
`-O1` y `-O2` — exactamente lo que la regla 3 prohíbe. El diseño a seguir:

1. Detección de bucles naturales sobre el CFG (preheader, header, cuerpo, back-edge), que hoy no
   existe y que O3 necesita igualmente.
2. Reconocimiento de las formas canónicas: copia (`dst[i] = src[i]`), relleno (`dst[i] = c`) y
   búsqueda (`while (*s) s++;`, `strcmp`, `memchr`).
3. Bajada a una expansión palabra-a-palabra con cola de bytes (la prueba `(w - 0x01010101) & ~w &
   0x80808080` de `asm/string_fast.casm`), o a una llamada a una rutina del propio compilador emitida
   en CASM, nunca a `memcpy` de la biblioteca (que puede no estar enlazada).
4. Tras el flag `-floop-idioms` (ON en O1, OFF en O0), con un banco de pruebas que compare los tres
   niveles y, si es posible, las cifras de instrucciones medidas por el Timer que documenta
   `asm/string_fast.casm`.

La referencia de rendimiento es la que da la propia STDLIB: sus versiones C a `-O2` son ~3× más
lentas que su asm a mano para `strcpy`/`strcmp`/`strchr`/`memchr` (§10.5).

### Pasada de revisión sobre la Ola 1

Tras cerrar las cuatro partes se ejecutó `open-code-review` sobre el diff completo
(`5a26a2e..HEAD`): 34 ficheros revisados, 26 hallazgos. El más importante fue **crítico** y está
corregido:

- **O1, sesgo de la división con signo (crítico).** El sesgo se calculaba como `x >>u (32-k)`, que
  son los *k* bits altos de `x`, no su signo; es `2^k-1` solo cuando esos bits son todos unos, así
  que para dividendos de magnitud grande redondeaba mal (p. ej. `(2^30+3)/4` daba `268435457` en vez
  de `268435456`). Se corrigió a `(x >>s 31) >>u (32-k)` y se añadió a `examples/20_divmod.c` un
  bloque de dividendos grandes (2^30, INT_MAX, INT_MIN+1, …) cuyos valores ahora se comprueban a mano
  en los tres niveles.

Otros hallazgos corregidos: el driver ya no resuelve `-l` sin `--run` ni debilita el guardia de
"no input files"; el `sysroot/lib` se busca *después* de `-L` (coherente con `sysroot/include`);
se usa la sobrecarga con `std::error_code` de `is_regular_file`; `deprecated`/`warn_unused_result`
en posiciones sin función se reportan con el mensaje genérico; los atributos de función en posición
de especificador sobre algo que no es función ya se avisan en vez de perderse; `warn_unused_result`
sobre una función `void` no avisa; un prototipo `noinline` no anula un `always_inline` de la
definición; `__has_include` malformado produce un solo diagnóstico y respeta el límite de token;
la DCE de una llamada `pure` retira también sus `Param`; y se centralizó en `ast/expr.h` el predicado
`builtinTouchesFloatBank`, del que derivan codegen y el sondeo del banco flotante del handler.

Quedan sin corregir dos hallazgos de severidad baja, anotados aquí: `typedef` con un atributo de
función se descarta en silencio (mismo hueco que la posición de especificador, que sí se corrigió), y
`builtinFromName()` enumera la lista de nombres a mano junto a `builtinName()` (sin `static_assert`
que impida que las dos deriven).

---

## 17. Ola B — calidad de código y observabilidad

| Parte | Estado | Commit |
| --- | --- | --- |
| **O13** — niveles `-Os`/`-Og` y `--stats` | **hecha** | `Add -Os/-Og optimization levels and --stats` |
| **O2** — eliminación de subexpresiones comunes (CSE) | **hecha** | `Add common subexpression elimination` |
| **O14 (parcial)** — eliminación de almacenamientos muertos por sobreescritura | **hecha** | `Extend dead-store elimination to overwritten stores` |
| **O3**, **O4**, **O5**, **O8**, **O9**, **O10**, **O11**, **O12** | **aplazadas, con motivo** | — |
| **F4**, **F8**, **F13** | **aplazadas, con motivo** | — |

**O12 se completó después.** Los builtins `imin`/`imax`/`umin`/`umax` ya existían; el
reconocimiento del patrón (`a < b ? a : b` → `imin`, `x < 0 ? -x : x` → `abs`) se hizo en `IrBuilder`
sobre el AST, que es donde el diamante todavía no existe. El match exige que cada brazo sea
estructuralmente uno de los operandos de la comparación (y libres de efectos), así que un ternario
con llamadas o con `==` conserva el diamante.

**Por qué se aplazan las restantes.** No son cambios de una tarde, y entregarlas a medias arriesga
miscompilaciones entre niveles (lo que la regla 3 prohíbe):

- **O3/O15 (bucles)** y **O11 (SCCP)**: necesitan análisis de bucles naturales y de dominancia, que
  este IR no calcula hoy. Son la base de la ganancia de rendimiento medida en la STDLIB (~3× en
  `strcpy`/`strcmp`/`strchr`/`memchr`), y merecen su propia ola.
- **O4/O5 (calidad del asignador)**: reasignar registros de forma agresiva (linear scan/coloring,
  coalescing) toca el conjunto callee-saved que asume `asm/setjmp.casm`; requiere un test de ABI
  antes, no después.
- **O8 (PC-relativo)**, **O9 (layout de bloques)**, **O10 (inlining)**, **O12 (patrones min/max/abs)**:
  viables, pero cambian muchas golden de CASM o dependen de que el ensamblador exponga la forma
  PC-relativa; se dejan para una ola centrada en el back-end.
- **F4 (setjmp nativo)**, **F8 (info de depuración de C)**, **F13 (LTO)**: necesitan, respectivamente,
  builtins de ABI, hablar el formato de depuración de CeresASM, o un IR serializable entre unidades.

### Pasada de revisión sobre la Ola B

`open-code-review` sobre `033fdb2..HEAD` (8 ficheros, 7 hallazgos). **Dos de severidad alta, ambos en
el CSE nuevo y ambos reales**, corregidos:

- el CSE numeraba y reutilizaba temporales sin comprobar que tuvieran **una sola definición**; este IR
  no es SSA y reutiliza ids (`materializeBoolean`, ternarios), así que una redefinición invalidaba una
  entrada y podía sustituir por un valor ya cambiado. Ahora solo numera un resultado con exactamente
  una definición y cuyos operandos también la tengan (el mismo guardia que `propagateCopies`);
- `useBlocks[result] <= 1` no probaba que el único uso estuviera **en el bloque actual**; un temporal
  vivo fuera de su bloque se habría dejado sin definición. Ahora se registra el bloque (único, ninguno
  o varios) de cada uso y solo se descarta si el uso está en el bloque actual o no existe.

También corregidos: el `--stats` contaba las instrucciones *después* de inlining (ahora antes, como
dice su comentario) y podía imprimir `(-0%)` para un crecimiento (ahora un delta con signo); se
vuelca el impresor de diagnósticos antes de la línea de `--stats`; `jumpTables` se asigna en vez de
acumularse con `++`; y la clave del CSE es ahora un POD con hash en lugar de una `std::string` por
instrucción (GlobalAddr deja de numerarse, que además evita comparar nombres por una clave POD).

Se añadió `examples/22_cse.c` (una expresión pura repetida junto a un ternario repetido) para cubrir
de extremo a extremo la forma que los dos fallos altos podían romper.

---

## 18. Ola C — profundidad de lenguaje

| Parte | Estado | Commit |
| --- | --- | --- |
| **F9** — rangos en `case` (`case lo ... hi:`) | **hecha** | `Add GNU case ranges` |
| **F10** — builtins del compilador (`__builtin_trap`/`unreachable`/`expect`/`constant_p`) | **hecha** | `Add __builtin_trap/unreachable/expect/constant_p` |
| **F2** — aritmética marcada (`__builtin_add/sub/mul_overflow`) | **hecha** | `Add __builtin_add/sub/mul_overflow` |
| **O7** — llamada de cola (`return f(args)` → epílogo + `jp`) | **hecha** | `Turn a returned call into a tail call` |
| **F4** — contrato de ABI de `setjmp`/`longjmp` verificado | **hecha** | `Pin the setjmp/longjmp callee-saved ABI with tests` |
| **O10** — inlining multi-bloque y con llamadas | **hecha** | `Inline multi-block callees and callees that call` |
| **F3**, **F6**, **F7**, **F12**, **F14** | **aplazadas, con motivo** | — |

**Por qué se aplazan.** Igual que en la Ola B, cada una es un proyecto en sí:

- **F3 (enteros de 64 bits)**: emulación en pareja de registros con `ADDC`/`SUBC`, layout de tipos,
  promociones y ABI de 64 bits; es el mayor de los aplazados.
- **F6 (alloca/VLA/FAM)** y **F7 (bitfields/packed)**: cambian el layout y el esquema de frame.
- **F12 (preprocesador)**: `#line` rompe la premisa de que las ubicaciones vienen del `LineMap`;
  `#include_next` necesita recordar de qué directorio vino cada fichero; `_Pragma` es un operador de
  macro.
- **F14 (`__asm__` con operandos/clobbers)**: cambia el contrato de ensamble en línea.

### Pasada de revisión sobre la Ola C

`open-code-review` sobre `85932a4..HEAD` (15 ficheros, 8 hallazgos). **Un fallo alto, real**, en F2:

- el tercer operando de `__builtin_*_overflow` es un **valor puntero**, no un lvalue del que tomar
  dirección; `lowerAddress()` sobre un `NameExpr` daba la dirección del *propio objeto puntero*, así
  que `int* p; __builtin_add_overflow(a, b, p)` escribía en `p` en vez de a través de él. Se corrigió
  a `lowerExpr()` (que sirve para las dos formas: `&r` da la dirección, `p` da el puntero cargado).

También corregidos dos desbordamientos/UB de enteros con signo y una complejidad cuadrática en el
manejo de rangos: los bucles que expanden un rango (en sema y en el `IrBuilder`) ahora iteran por
**desplazamiento** en vez de por valor, porque `high` puede ser `INT64_MAX` y `++valor` desbordaría;
los valores ya vistos de un `switch` pasan de `std::vector` con `std::find` a `std::unordered_set`
(un rango de 65536 valores ya no hace cuadrática la detección de duplicados); y la signatura de la
operación de overflow en F2 se deriva de **ambos** operandos (conversión aritmética usual), no solo
del primero, para no depender del orden. Además se endureció un test de codegen débil (`"add"`
también casaba con `"addi"`) y se anotó en `examples/24` que `trap`/`unreachable` no pueden
ejercitarse en un ejemplo que imprime.

---

## 19. Backlog priorizado (lo que queda)

Ordenado por **posibilidad de resolverlo sin dependencias** primero y, dentro de eso, por valor.
Cada fila indica qué lo bloquea. Se implementa de arriba abajo, un commit por fila, con una pasada de
`open-code-review` cada 3–4 commits.

| # | Item | Depende de | Estado |
| --- | --- | --- | --- |
| 1 | **F14a** — `__builtin_stack_pointer()` (estado de máquina) | — | **hecho** |
| 2 | **F12** — `#include_next` (hecho); `#line` y `_Pragma` siguen pendientes | — | parcial |
| 3 | **O9** — layout de bloques para fall-through | — | **hecho** |
| 4 | **O5** — coalescing de copias en el asignador | test de ABI (F4) | **aplazado** (subsumido por copy propagation + DCE; el coalescing real pertenece a O4) |
| 5 | **O8** — acceso PC-relativo a estáticos (`LDRP`/`STRP`) | ensamblador | **bloqueado** (CASM no expone `LDRP`/`STRP`) |
| 6 | **O7** — llamada de cola + `BL`/`BLR` | — | **hecho** (solo `jp`; `BL`/`BLR` cambian el epílogo por sitio de llamada y se descartan) |
| 7 | **F4** — `setjmp`/`longjmp` nativo (o contrato de ABI verificado) | — | **hecho** (contrato verificado: `setjmp` en CASM a mano + golden de los máscaras callee-saved) |
| 8 | **O12** — builtins `imin`/`imax`/`umin`/`umax` **hechos**; reconocimiento de patrones `min`/`max`/`abs` **hecho** | — | **hecho** |
| 9 | **O10** — inlining multi-bloque y con llamadas | — | **hecho** |
| 10 | **O11** — plegado de autocomparación **y** SCCP (lattice + worklist, aristas tomadas, `switch` constante resuelto) | — | **hecho** |
| 11 | **F6** — *flexible array members* **hechos**; `alloca`/VLA aplazados (necesitan re-basar el frame en `fp`) | — | parcial |
| 12 | **F7** — bitfields y layout empaquetado | — | pendiente |
| 13 | **O3** — LICM e IV-SR **hechos** (detección de bucles naturales + dominancia; `base + i*C` → puntero incremental) | — | **hecho** |
| 14 | **O15** — reconocimiento de idiomas de bucle byte→palabra | O3 | **cerrado** (relleno, copia y búsqueda a nivel de bucle; `strcpy`/`strcmp`/`strchr` quedan fuera: son de función completa) |
| 15 | **O4** — mejor asignador de registros | contrato de `setjmp` (F4) | pendiente |
| 16 | **F3** — enteros de 64 bits | ABI de 64 bits | **F3.1a/F3.1b/F3.2/F3.3/F3.4 hechas** (tipo, representación, aritmética, mul/div/mod, shifts, conversiones y ABI ancha); F3.5 pendiente (STDLIB) |
| 17 | **F8** — información de depuración de C | formato de debug de CeresASM | pendiente |
| 18 | **F13** — LTO / IR de programa completo | serialización de IR | pendiente |

### Revisiones de este backlog

- **Batch 1** (`#include_next`, `__builtin_stack_pointer`, layout de bloques): un hallazgo alto (el
  layout podía empeorar el `if (cond) { body }` común) y varios medios, todos corregidos — ver el
  commit de correcciones.
- **Batch 2** (plegado de autocomparación, builtins `imin`/`imax`/`umin`/`umax`): una rama muerta
  retirada y dos huecos de test cubiertos (la tabla de verdad completa y el caso `float`).
- **Batch 3** (`O7` llamada de cola, `F4` contrato de ABI, `O12` patrones `min`/`max`/`abs`): un
  hallazgo alto real, corregido — el plegado `a < b ? a : b` → `imin` descartaba una lectura
  `volatile` (el ternario lee el operando elegido dos veces, una en la condición y otra en el brazo,
  y el builtin una sola), así que `exprsEquivalent` ahora rechaza un `NameExpr` volátil. Corregidos
  además: el flag `tailCalls` se movió a la sección de codegen de `optimization.h` (que es quien lo
  lee), `tryLowerSelectIdiom` toma su nodo por `const&`, y se añadieron tests de los guards del tail
  call (llamada indirecta y ensamble en línea), del tail call flotante, del ternario `float`/puntero,
  de los espejos no estrictos de `abs` y de los operandos `volatile`. Descartado por nimio el
  hallazgo de que `findTailCall` repite la validación de argumentos del camino de emisión (solo
  divergen ante IR malformado, que `IrBuilder` no produce).
- **Batch 4** (`O10` inlining multi-bloque, `O11` SCCP, `F6a` *flexible array members*): 13 ficheros,
  4 hallazgos, todos corregidos. Uno medio de corrección: un *flexible array member* se aceptaba en
  una `union` (el mismo `StructDecl` cubre las dos, y el miembro pasaba el test de "último campo"),
  donde C lo prohíbe porque todos los miembros de una `union` empiezan en el offset 0; ahora se
  rechaza con `E3101`. Otro medio, de rendimiento: `containsFlexibleArrayMemberByValue` no memoizaba
  los resultados negativos, así que una jerarquía en forma de DAG re-exploraba cada sub-struct una
  vez por hermano (exponencial en profundidad); ahora usa el mismo `noCycleMemo` que
  `containsByValue`. Corregidos además: un array de un struct con FAM a nivel de objeto
  (`struct S arr[2];`) no se diagnosticaba (ahora se comprueba en `visit(ast::VarDecl&)` con
  `arrayElementHasFlexibleArrayMember`), y `E3099`/`E3100`/`E3101` faltaban en el catálogo de
  `docs/11-Diagnostics.md`.

- **Batch 5** (`O3` LICM): 4 ficheros, 4 hallazgos, todos corregidos (severidad baja). Dos de
  mantenibilidad: `atO2Without` duplicaba `without()` en los tests de codegen (ahora la reusa), y la
  justificación del orden "de dentro hacia fuera" de los bucles era falsa (`invariant()` rechaza
  cualquier operando definido en el bucle, incluido un preheader interior, así que el orden no cambia
  nada; se retiró el `sort` y se corrigió el comentario). Uno de rendimiento real: `Const` no era
  elevable, así que `a * 2` en un bucle no se elevaba porque el `2` lo materializa `IrBuilder` dentro
  del bucle; ahora una constante literal definida en el bucle es invariante y se eleva **junto con**
  la instrucción que la lee. Eso, a su vez, rompió el plegado `cmp; const 0; br.ne` del que depende
  la fusión comparación-rama de codegen (el cero se separaba de la rama), así que la elevación de
  constantes es **a demanda**: solo se mueve una constante cuando otra instrucción elevada la
  necesita, y una constante que alimenta una rama nunca se mueve. Se añadieron tests para el caso de
  la constante literal, la exclusión de `FloatToInt` y el bucle con llamada.

- **Batch 6** (`O3` parte 2, IV-SR): 5 ficheros, 7 hallazgos, todos corregidos. **Uno alto y real**:
  `decomposeScaled` aceptaba la carga del contador a cualquiera de los dos lados de un `<<`, pero el
  desplazamiento no es conmutativo; `p[1 << i]` (elemento de un byte, `d = base + (1 << i)`) se
  reescribía como si fuera `i * 2`, o sea un puntero que avanza `step * 2` — una miscompilación
  silenciosa. Ahora la carga solo se acepta a la derecha de un `mul` (conmutativo); a la izquierda
  del `shl` va el contador y a la derecha la constante. Se añadió el test de regresión. Uno medio:
  `p.step * p.scale` podía desbordar un producto i64 con signo (UB en el propio compilador); ahora el
  delta se calcula en aritmética u64 módulo 2^32 —el ancho del puntero— y se extiende con signo para
  que un `-4` se lea como tal. Uno de rendimiento: el pase no ayuda si `-fno-dce` deja la
  multiplicación muerta en su sitio, así que ahora exige `deadCodeElimination` y lo documenta. Y
  cuatro bajos: la descripción del flag decía "advanced by C" en vez de por el paso por C; faltaban
  tests del paso negativo, del caso escala 1 (`char[]`, sin multiplicación) y del apagado en `-Og` /
  encendido en `-Os`, los tres añadidos.

- **Batch 7** (`O15` parte 1, idioma de relleno): 8 ficheros, 8 hallazgos, todos corregidos. **Tres
  de corrección reales**, los dos primeros miscompilaciones silenciosas: (1) alto — el pase no
  comprobaba que el contador fuera *no escapado* (a diferencia de IV-SR); si `&i` se guardaba en un
  puntero `q` y `*q` se leía después del bucle, la reescritura dejaba `i = 0` en vez de `n`. Ahora
  exige `nonEscaping[counterLocal]`. (2) medio — si el `store` de relleno y el del contador comparten
  el bloque del *latch* (`while (i < n) { i = i + 1; p[i] = c; }`), el relleno lee el contador ya
  incrementado y cubre `[1, n+1)`, no `[0, n)`; ahora se exige que el `store` de relleno preceda al
  del contador. (3) bajo — el preheader se elegía por "un solo sucesor", que un `CondJump` degenerado
  con ambos brazos iguales también cumple; leer su payload como `IrJumpPayload` con `as<>()` lanzaba
  dentro de una función `noexcept`. Ahora el terminador se reconstruye siempre como un `Jump` nuevo.
  Uno de rendimiento: la ruta no alineada de `__cc_memset` se quedaba en el bucle de bytes para todo
  el rango (igual que el `memset` de la STDLIB); ahora alinea con bytes y cae al bucle de palabras,
  que es lo que el comentario decía. Los cuatro restantes bajos: el orden del listado de pases en
  `ir_optimizer.h` no coincidía con el real; faltaba anotar que el bucle muerto solo lo retira
  `unreachable-block-elimination`; y tests que faltaban (contador no escapado, contador leído tras el
  bucle, incremento antes del `store`, relleno volátil, cota constante, y las rutas del guardia y del
  *tail* del propio `__cc_memset`).

- **Batch 8** (`O15` parte 2, copia): 7 ficheros, 2 hallazgos, ambos corregidos (severidad baja). Uno
  real aunque raro: la prueba de no-solape de `__cc_memcpy` calculaba `s + n`, que da la vuelta a 32
  bits si el rango origen acaba en lo alto del espacio de direcciones; entonces `ifae d, s+n` daba por
  disjuntos unos rangos que se solapan y se tomaba la copia por palabras, perdiendo la repetición de
  bytes que define el bucle C. Ahora se compara `d - s >= n`, que no desborda porque en ese punto
  `d > s`. El otro, de rendimiento: `__cc_memcpy` se iba a bytes si *cualquiera* de los punteros
  estaba desalineado, perdiendo la palabra a palabra en el caso común de dos punteros con la **misma**
  desalineación (`copy_bytes(b + 2, src + 2, n)`); ahora, cuando `(d ^ s) & 3 == 0`, copia los bytes de
  cabeza que alinean a ambos y cae al bucle de palabras, y solo las desalineaciones distintas van a
  bytes.

- **Batch 9** (`O15` parte 3, búsqueda): 7 ficheros, 4 hallazgos, todos corregidos. **Dos críticos,
  ambos miscompilaciones silenciosas**: (1) `__cc_memchr_index` no tenía el guardián de contador no
  positivo; como `ifbl` es sin signo, un `n` negativo entraba al bucle de palabras y leía fuera del
  búfer (`for (i = 0; i < n; i++)` devuelve 0 para `n <= 0`). (2) la forma `strlen` se reconocía por
  el `Cmp` interno del header sin comprobar contra qué se comparaba, así que `while (s[i] != c)` (un
  "busca el primer `c`" a mano) se bajaba a `__cc_strlen` y devolvía la longitud en vez del índice.
  Uno medio: `searchByte` se tomaba sin restricción de anchura, pero la rutina trunca a 8 bits y
  compara el byte del array extendido a cero; un `int c` sin cast comparado a ancho de palabra no es
  equivalente (bucle y rutina discrepan para `c` fuera de rango o ≥ 128). Ahora se exige que el valor
  sea de un byte y de la misma signedness que el elemento (o un literal en su rango). Uno de
  rendimiento: `__cc_memchr_index` se quedaba en bytes para todo el rango si la base estaba
  desalineada; ahora alinea con bytes y cae al bucle de palabras. Tests de regresión para los dos
  críticos y para el ancho, más los casos `n = 0` y `n < 0` en el ejemplo —que de hecho cazaron un
  fallo del propio arreglo: el guardián saltaba a `.cmi_none` antes de que `r7` tuviera la base.

- **F3.1a** (mitad de tipos de los enteros de 64 bits): 13 ficheros, sin hallazgos de `ocr` todavía
  (la pasada de revisión toca cada 3–4 commits). `long long`/`unsigned long long` son ya
  `TypeKind::LongLong`/`ULongLong`, de 8 bytes y alineación 8: el parser los entrega como tales (sin
  `W2001`), `sizeof`/`alignof`/layout de struct y `_Static_assert` ven la anchura real, y los
  literales aceptan el sufijo `ll`/`LL` (y `ull`/`llu` en cualquier orden), que les da el tipo de 64
  bits. `double`/`long double` siguen capados a `float`, que es F3 solo de enteros. Las conversiones
  aritméticas usuales sitúan los dos tipos por encima de todos los de 32 bits (`unsigned long long` >
  `long long` > `unsigned long` > `long` > `unsigned int` > `int`), en `sema.cpp` y en la copia que
  `ir_builder.cpp` mantiene. La mitad de *representación* (el IR es de 32 bits, así que un valor de 64
  bits se legaliza a una pareja `(lo, hi)` en cada operación, load/store y llamada) la cubren
  F3.1b–F3.4; solo queda la STDLIB (F3.5). `IrBuilder` ya no rechaza un `long long` bajado
  (`rejectWideInteger()` se retiró); siguen rechazados con `E5002` un discriminante de `switch` ancho
  (F9) y un operando ancho de un builtin de una instrucción.

- **F3.1b** (representación y aritmética básica de los enteros de 64 bits): se eligió la opción **(B)**
  del briefing —un valor de 64 bits baja a la **dirección** de sus 8 bytes, palabra baja en +0 y alta
  en +4, exactamente como un struct o un array— porque reutiliza la maquinaria de memoria compuesta
  (`emitMemoryCopy`, la convención "un agregado es su dirección") y no añade ningún opcode al IR: todo
  consumidor o copia los bytes o carga las dos palabras. Cubre load/store/copia/asignación (variable,
  campo, elemento, global), `+`/`-` con el acarreo/préstamo a través de `Cmp` (no hay `ADDC` en el IR),
  `&`/`|`/`^`/`~`/`-` unarios, comparaciones (alta primero, baja sin signo), `++`/`--`, un `?:` ancho,
  la condición ancha de `if`/`while` y la conversión `int`↔`long long`. La representación se apoya en
  `materializeWide`/`emitWideStore`/`loadWideWord`/`makeWideValue`/`lowerWideArithmetic`/
  `lowerWideCompare`/`toWord`. Siguen **rechazadas con E5002** (y documentadas) la multiplicación,
  división y módulo (F3.2), los desplazamientos y las conversiones `float`↔`long long` (F3.3), y todo
  valor de 64 bits que cruce una frontera de función (F3.4: parámetro, retorno o argumento). También se
  rechazan, por caer en la misma frontera, un discriminante de `switch` ancho (F9) y un operando ancho
  de un builtin de una instrucción (F3.3). Un índice de array, un desplazamiento de puntero o un
  contador de shift anchos se truncan a palabra (lo que C convierte a `int`). `examples/35_int64.c`
  fija a mano, a los tres niveles, el acarreo/préstamo (incluido `-0x100000000`), el signo compartido
  de las comparaciones, un campo de struct, un array y una variable global.

- **Revisión de `ocr` sobre F3.1a+F3.1b** (`eabd092..71ab3d8`): 21 ficheros, 13 hallazgos. **Dos altos
  y dos medios, todos reales, corregidos**:
  (1) *alto* — el post-incremento/decremento ancho devolvía el valor NUEVO (`long long b = a++;`
  dejaba `b == a + 1`): `_lastValue` apuntaba al almacenamiento que el `emitWideStore` acababa de
  sobrescribir, así que ahora se copian las dos palabras viejas a un temporal antes del store, como
  ya hacía el camino escalar cargando en un registro.
  (2) *alto* — `emitWideStore` no rechazaba un origen `float`, y el inicializador y la asignación le
  pasan el tipo del origen directamente (sin `convertForStore`), de modo que `long long x = 1.5;`
  reinterpretaba los bits del float como palabra baja; el guardián de `float`→64 bits vive ahora en
  ese único cuello de botella, que es por donde pasa todo store ancho.
  (3) *medio* — convertir un ancho a `bool` miraba solo la palabra baja (`bool b = 0x100000000LL` daba
  falso); ahora es `(low | high) != 0`.
  (4) *medio* — `toWord()` se aplicaba también a un resultado `float`, truncando en silencio
  `long long a; float f = a + 1.5f;`; un operando ancho en una expresión `float` es la conversión de
  F3.3 y ahora se rechaza.
  Bajos corregidos: el sufijo de caso mezclado `lL`/`Ll` ya no se consume (C exige `ll`/`LL`), tests de
  la ruta de base (hex/binaria) y del caso mezclado, dos comentarios obsoletos (`token.h`, el e2e), un
  `{` mal colocado en un test, y el ejemplo `35_int64.c` gana el caso `^`, una variable global ancha,
  el caso post-incremento (que era el bug (1)), `(bool)0x100000000LL` (el bug (3)) y las notas de orden
  de bytes y del campo `tag`.

- **F3.2** (multiplicación, división y módulo de 64 bits): el producto completo se compone de las
  piezas de 32 bits —`lo = low32(al*bl)`, `hi = high32(al*bl) + low32(ah*bl) + low32(al*bh)`— usando
  el `mul` de 32 bits y el builtin `mulhu` (que ya existía de F1); el patrón de bits bajo de un
  producto con signo es el mismo que el sin signo, así que no hace falta `imulh`. La **división y el
  módulo** sí necesitan una rutina: un bucle restaurador de desplazamiento y resta de 64 pasos, que
  `codegen` emite **una sola vez** al final de `@text` cuando una división aparece (el mismo mecanismo
  de rutina emitida de O15, con etiquetas *file-level* privadas). El IRBuilder baja `a / b` y `a % b`
  a una llamada a `__cc_div64` con `dest`, `&a`, `&b` y `firmado` (cuatro punteros/enteros, porque la
  ABI de 64 bits es F3.4); la rutina deja el cociente en `[dest]` y el resto en `[dest+8]`, así que
  `/` y `%` comparten una sola llamada. Usa la cadena de acarreo de la ISA (`adc`/`sbc`) para el
  doble-carry y la resta entre palabras, convierte a magnitud y reaplica los signos al final (división
  truncada hacia cero de C), y `pushm 0x0F00`/`popm` salvan r8-r11. Un divisor cero (UB en C) guarda
  cero en vez de dar 64 vueltas. `examples/35_int64.c` fija a mano producto pequeño y grande, `/` y
  `%` con y sin signo, y un cociente negativo; además se validó contra una referencia de Python con
  ~1700 casos aleatorios a los tres niveles (sin incluir en el repo, por ser un banco de pruebas).
  Siguen fuera de F3 los desplazamientos y las conversiones con `float` (F3.3) y la ABI ancha (F3.4).

- **Revisión de `ocr` sobre F3.2** (`71ab3d8..2d5ac5a`): 10 ficheros, 9 hallazgos, **ninguno crítico
  ni alto**, dos medios, todos corregidos:
  (1) *medio* (documentación, no código) — la cabecera de `examples/35_int64.c` seguía diciendo que
  `*`/`/`/`%` se rechazan, obsoleto en el mismo commit que los añade; reescrita junto con la
  justificación del `union` (ya no es «evitar una división de 64 bits») y la frase «el valor solo es
  un local» (ahora hay un global).
  (2) *medio* (test) — el test de codegen de `__cc_div64` solo comprobaba la presencia por substring;
  ahora comprueba también `popm` (que el `pushm`/`popm` cuadra), que un programa con `/` y `%` emite
  la rutina **una sola vez** y hace dos llamadas, y que un programa que no divide no la lleva.
  Uno *bajo* de corrección: el `a++`/`a--` ancho sobre un `volatile long long` leía el objeto dos veces
  (el snapshot y otra vez para calcular), cuando la fuente implica una sola lectura; ahora se lee una
  vez a un temporal y la aritmética trabaja sobre él. Bajos restantes: comentario a medias en la rama
  de signo del cociente, máscara `pushm`/`popm` como literal repetido (ahora `kDiv64SaveMask`),
  `emitEmittedRoutines` renombrado a `emitCarriedRoutines`, y un comentario del e2e con la fase
  equivocada (`F3.2-F3.4` → `F3.3-F3.4`).

- **F3.3** (desplazamientos y conversiones con `float`): los shifts (`<<`, `>>`, aritmético y lógico)
  se bajan a un **árbol de tres ramas** sobre el contador (0, 1..31, 32..63), porque un shift de 64
  bits son dos de 32 y la ISA enmascara el contador a 5 bits; se materializa la dirección del resultado
  y las constantes antes de ramificar para que dominen todas las ramas. Las conversiones
  `int`↔`long long` ya estaban; `long long`↔`float` se hacen **de dieciséis bits en dieciséis** (Horner
  para ir a `float`, descomposición con escalas 2^48/2^32/2^16 para volver), igual que el
  `ns64_to_float` de la STDLIB. La conversión `long long`→`float` queda a menos de 1 ULP del redondeo
  correcto (el f32 de la máquina no siempre puede más con una fuente de 64 bits); `float`→`long long`
  es exacta en rango. La comparación de un ancho contra un `float` convierte el ancho entero vía
  `toFloatIfNeeded`. Un literal decimal `LL` fuera del rango de `long long` (p. ej.
  `18446744073709551615LL`) ahora **avisa** (`W0015`) y conserva su patrón de 64 bits (antes no avisaba
  y quedaba en 0), y un `ULL` de todo el rango no avisa. Se validaron 384 casos de shift y ~90 de
  conversión contra una referencia de Python a los tres niveles (banco de pruebas, fuera del repo).
  Queda fuera de F3 solo la ABI ancha (F3.4: parámetro, retorno y argumento de 64 bits) y la STDLIB
  (F3.5); un discriminante de `switch` ancho (F9) y un operando ancho de un builtin de una instrucción
  siguen rechazados con E5002.

- **Revisión de `ocr` sobre F3.3** (`2d5ac5a..da1dbfb`): 13 ficheros, 13 hallazgos, **dos altos y tres
  medios, todos reales, corregidos**:
  (1) *alto* — al pasar el tipo del **destino** como `fromType` de `emitWideStore` (inicializador,
  asignación y las dos ramas del ternario, cambios de F3.3), la volatilidad que gobierna las cargas
  pasó a ser la del destino: un `volatile long long s; long long d = s;` leía `s` con cargas no
  volátiles, que el optimizador puede eliminar o reenviar. Ahora una fuente ancha se copia como su
  propia dirección (su volatilidad en las cargas) y solo una fuente escalar o `float` se convierte
  primero y pasa el tipo del destino.
  (2) *alto* — el mismo problema en la asignación y en el ternario; corregido con el mismo reparto.
  (3) *medio* — `long long x = 1; x += 1.5f;` se rechazaba con E5002 (el tipo promovido era `float`)
  mientras `x = x + 1.5f;` ya bajaba; el camino de asignación compuesta ahora convierte el resultado
  promovido de vuelta al tipo ancho.
  (4) *medio* — `(unsigned long long)f` con `f` negativo generaba una rama inalcanzable (la negativa)
  y un slot de frame de más; un valor negativo a un destino sin signo es UB, así que ahora devuelve la
  magnitud directamente sin crear ramas.
  (5) *medio* — un literal `ll`/`LL` fuera de rango en base 16/2 (`0xFFFFFFFFFFFFFFFFLL`) avisaba con
  W0015, pero C lo admite como `unsigned long long`; el aviso queda restringido a base 10.
  Uno *bajo* de corrección: una fuente ancha `volatile` convertida a `float` se leía varias veces
  (prueba de signo + conversión); ahora se lee una sola vez a un temporal. Los bajos de test: el
  ejemplo `35_int64.c` ejercía los desplazamientos con operandos cuyo término de cruce se perdía
  (`a << 4` con `a` de palabra baja 2) y `(-1) >> 1` era un shift de `int`, no de 64 bits; ahora usa
  `0x00000001F0000002LL << 4`, `(-1LL) >> 36`, `(unsigned)(-1LL) >> 36` y un `>>` pequeño sobre
  `0x0000000100000002 >> 4`, con los tres brazos (0, 1..31, 32..63) cubiertos; el test de codegen
  reutiliza `countOf` en vez de duplicarlo y cuenta `__cc_div64:`/`div_loop:` en vez de un `pushm` que
  también emite una función con llamadas; y el test de IR cubre los bordes 0/1/32 de los shifts.

- **F3.4** (convención de llamada de 64 bits): un valor ancho cruza una frontera de función como la
  misma pareja de dos palabras que ya representa en memoria. Un **parámetro** ancho llega en **dos
  registros de argumento consecutivos** (r0/r1 para el primero, r2/r3 para el segundo), y si el banco
  de cuatro no tiene dos libres, la pareja entera cae a **dos palabras de la pila saliente** (nunca
  una palabra en un registro y otra en pila, para que el callee siempre lea la segunda en
  `index + 1` del mismo sitio). Su *home* es un slot de 8 bytes (el único local de 8 bytes, porque un
  ancho es siempre una pareja en memoria, nunca en registro) que se escribe bajo-alto a través de una
  única dirección. Un **argumento** ancho viaja como la dirección de su pareja: el llamador carga las
  dos palabras a los dos registros (o a las dos palabras de pila) desde esa dirección. Un **retorno**
  ancho vuelve en **ret0/ret1** (r0 la baja, r1 la alta); el llamador guarda las dos palabras en un
  temporal de 8 bytes recién creado, que es el valor de la `CallExpr`. Se retiran los rechazos
  `E5002` de firma/llamada/retorno que F3.1b–F3.3 mantenían como guardián. El **inlining** de una
  función ancha se desactiva en `isInlinable` (el *splice* no modela una pareja de resultado ni un
  `ret` que devuelve dos valores), y la **llamada de cola** también se descarta cuando hay resultado o
  argumento ancho (el `jp` deja el resultado solo en r0 y no mueve una pareja). `IrParamPayload.isWide`,
  `IrCallPayload.resultHigh`/`hasWideResult` y `IrReturnPayload.highValue`/`hasWideValue` llevan la
  convención por el IR sin añadir opcodes; `assignArgSlots` pasa a devolver **una entrada por
  argumento** (con bandera `wide`, ocupando dos slots), que es lo que alinea llamador y callee.
  `examples/35_int64.c` gana `widen`, `add3` (tercer ancho a la pila) y `sum6` (los seis por pila),
  pasados y devueltos por valor; y se validó con bancos aleatorios de Python (20 casos de ABI mixta,
  7 de suma de 1..7 anchos, 60 de formas int/float/ancho aleatorias) a los tres niveles.

- **Revisión de `ocr` sobre F3.4** (`cdcde68..23a8b29`): 14 ficheros, 14 hallazgos (3 fallaron al
  ejecutarse), **dos altos y dos medios, todos reales, corregidos**:
  (1) *alto* — un argumento ancho pasado a un parámetro **estrecho** (`int f(int); long long x; f(x);`)
  se marcaba `wide` por el tipo del argumento y se convertía a su propio tipo ancho (no-op), así que
  la pareja se pasaba a un callee de una palabra y `f` leía la palabra baja de la *dirección*; ahora
  `isWide` lo decide el tipo del parámetro declarado (o, si el callee es desconocido, el del
  argumento), y la conversión usa siempre `paramType`, que trunca el ancho a su palabra baja.
  (2) *alto* — el guardián de inlining solo miraba la firma de la función, así que una función
  estrecha que **contiene una llamada ancha** (`int h(void){ long long v = id(1); ... }`) se cortaba
  con el splice, que remapea `p.result` pero no `p.resultHigh`: la palabra alta quedaba indefinida;
  ahora `isInlinable` rechaza cualquier cuerpo con un `Call.hasWideResult`.
  (3) *medio* — el callee derivaba la bandera `wide` del tamaño del slot mientras el llamador la
  derivaba de `isWide`; ambos lados coinciden ahora porque el builder la deriva del mismo tipo de
  parámetro (arreglo de (1)).
  (4) *medio* — `slotAddressOffset()` era código muerto y su forma `[sp + Frame.slotN + 4]` no
  ensamblaría (el prólogo ancho usa `la at, Frame.slotN` + `[at + 4]`); eliminado, `slotAddress()`
  vuelve a su forma original.
  Bajos corregidos: `secondResultOf()` era código muerto (retirado, con el comentario del campo
  corregido); un `return` de un lvalue `volatile long long` perdía el cualificador (dos cargas no
  volátiles donde esas son las únicas lecturas del objeto), ahora lo propaga; el comentario del
  diagnóstico `E5002` enumera también el guardián float→64 bits; y las notas de `examples/35_int64.c`
  (que decían que un ancho no cruza una frontera, que `widen` acarrea, que `sum6` va todo a pila y
  «una vez gastados los cuatro») se reescribieron para reflejar la convención real. Se añadió un test
  e2e de la ABI ancha a los tres niveles (que ejercita las ramas de *alias*/rotación del resultado y
  del retorno, alcanzables solo con asignación de registros), y se corrigió el mojibake (`Â§`→`§`,
  `Ã±`→`ñ`) que una edición anterior había introducido en `ir_builder.cpp` y `test_codegen.cpp`.

Los items 4–18 quedan pendientes. Los bloqueados o aplazados tienen su razón en la tabla; los demás
son proyectos de varios días (bitfields y layout empaquetado para F7; reasignación de registros para
O4/O5; `#line`/`_Pragma` para F12; representación y ABI ancha de F3, que ya tiene su mitad de tipos;
formato de depuración para F8; serialización de IR para F13).

**O3 se partió igual que F6.** El LICM ya está: detección de bucles naturales (aristas de retroceso
sobre un árbol de dominancia) y elevación de cargas/cálculos invariantes al preheader. Un bucle que
llama a algo se deja intacto a propósito: un valor elevado vive durante todo el bucle, o sea también
a través de la llamada, y el propio test del asignador para "¿este rango cruza una llamada?" es
lineal en orden de emisión, que una arista de retroceso engaña.

**La parte 2 de O3 (IV-SR) también está.** `strengthReduceInductionVariables` (`-finduction-vars`,
ON en O1, OFF en O0 y en `-Og`) reconoce `base + i*C` dentro de un bucle donde `i` es una variable
de inducción básica —un local no escapado, no volátil y de palabra con un único `store` en el bucle,
en el *latch* único, de `load(i) ± k`— y lo reescribe como un puntero incremental en un slot nuevo:
se inicializa en el preheader a `base + i_entrada*C`, se avanza `k*C` en el latch (detrás del propio
`store` de `i`) y cada uso de la dirección pasa a ser una carga de ese slot. Los bucles con llamada
se dejan igual que en el LICM (mismo problema de liveness lineal del asignador), y la forma exigida
es canónica: la base invariante definida antes del bucle, el operando escalado sin otros usos, el
valor derivado dominado por su definición y usado solo dentro del bucle y nunca en el latch. Además
se rechaza una base constante, porque si no el pase volvería a dispararse sobre el `+ offset` de un
acceso a miembro (donde no hay multiplicación que ahorrar) y crearía punteros inútiles.

El trozo de análisis de bucles que el LICM tenía incrustado se extrajo a `findNaturalLoops`, que
ambos pases comparten; es el único cambio que toca al LICM, y sus tests y goldens siguen verdes.
`examples/31_induction_vars.c` cubre los tres casos (stride 4, stride 12 no potencia de dos, y
recorrido inverso desde un índice no nulo) con valores calculados a mano, corriendo a los tres
niveles. Medido sobre el caso stride 12, el cuerpo pasa de `mul` + `add` + carga a una sola carga,
con el `add` de avance movido al latch: una instrucción menos por iteración y sin multiplicación.

**O15 empezó por el idioma de relleno.** `lowerFillIdioms` (`-floop-idioms`, ON en O1/Os, OFF en O0
y en `-Og`) reconoce `for (i = 0; i < n; i++) p[i] = c;` —contador local no escapado y de palabra,
inicializado a 0 antes del bucle y sumado 1 en el *latch* único; `n` y `c` invariantes; un único
`store` de byte, no volátil, a través de `base + i`; una sola salida, la rama falsa del header— y lo
sustituye por una llamada a `__cc_memset`, una rutina que **emite el propio compilador** al final del
`@text` de la unidad (nunca se enlaza de la biblioteca, así que un programa que usa el idioma siempre
la tiene y uno que no, no paga nada). La rutina es un símbolo *file-level* (privado), así que dos
unidades que usen el idioma llevan cada una su copia y el enlazador no ve duplicados; rellena palabra
a palabra una vez alineado el destino, con cola de bytes, y trata un contador con signo ≤ 0 como cero
bytes, que es lo que hacía el `for`. El reconocimiento es deliberadamente estrecho (nada de `break`,
de llamadas, de comparación sin signo ni de contadores que no empiecen en 0) y se apoya en el LICM
para elevar `c`, que llega estrechado; los cinco casos negativos están cubiertos por tests.
`examples/32_loop_fill.c` fija la salida a mano a los tres niveles, incluido el caso `n = 0`.

**La copia también está** (`lowerLoopIdioms`, el mismo pase y flag). `for (i = 0; i < n; i++) d[i] =
s[i];` se baja a `__cc_memcpy`, con la sutileza de que el bucle C es una **copia hacia delante por
bytes, bien definida aunque los rangos se solapen** (con `d > s` repite bytes, que es justo lo que
`memcpy`/`memmove` no reproducen). La rutina emitida copia palabras solo donde es demostrablemente
equivalente —`d <= s`, o rangos disjuntos— y cae a una copia hacia delante por bytes fiel en el
solape con `d > s`; un contador con signo ≤ 0 no copia nada. `examples/33_loop_copy.c` fija a mano
los tres casos (disjunto, solape `d > s`, solape `d < s`) a los tres niveles.

**La búsqueda también está** (`lowerSearchIdioms`, mismo pase y flag). Dos formas de bucle:
`while (s[i] != 0) i++;` (un `strlen` a mano) se baja a `__cc_strlen`, y `for (i = 0; i < n; i++) if
(s[i] == c) break;` (un `memchr` a mano) a `__cc_memchr_index`; ambas rutinas las emite el back end
y escanean palabra a palabra con la prueba `(w - 0x01010101) & ~w & 0x80808080` (y, para `memchr`, un
`xor` previo con `c` replicado). El contador *es* el resultado (la longitud, o el índice de la
coincidencia o `n`), así que el retorno de la rutina se guarda en su slot antes de salir del bucle.
A diferencia de relleno/copia, la búsqueda **no** exige una sola salida: el bucle de `memchr` sale
por el `break` y por la cota, y el pase solo exige que todas las salidas confluyan en el bloque que
lee el contador. Un bucle de búsqueda no tiene ningún efecto (ni un `store` salvo el del contador),
y `examples/34_loop_search.c` fija a mano seis casos (longitud, desplazamiento, encontrado/no
encontrado, cota corta) a los tres niveles.

Queda fuera de O15 lo que es **reconocimiento de función completa**, no de bucle: `strcpy`, `strcmp`
y `strchr` de la STDLIB devuelven un puntero o una diferencia y salen con `return` desde dentro del
bucle, así que su semántica no es "el contador es el resultado" y necesitaría otro pase (y un
guardían distinto). El nivel de bucle —relleno, copia y búsqueda— queda cerrado.

**F6 se partió en dos.** El *flexible array member* es autocontenido (parser, layout, `sizeof`,
acceso) y ya está hecho. `alloca`/VLA no lo son: el compilador direcciona todo el frame como
`[sp + Frame.slotN]`, y `sp` debe quedarse quieto entre `enter` y `leave` (24-Calling-Convention.md) —
moverlo para reservar en tiempo de ejecución invalidaría cada acceso a un local y el área de
argumentos salientes, que tiene que seguir en `[sp + 0]`. Hacerlo bien es re-basar los accesos al
frame en `fp` (r14) con un área saliente en el `sp` actual, un cambio de ABI que merece su propia
parte con su propio guardián de tests, no un añadido a la de layout.

---

## Anexo — Referencias de código clave

| Tema | Fichero / símbolo |
| --- | --- |
| Opcodes IR y payloads | `libs/ir/include/ceresc/ir/ir_instr.h` |
| Optimizador | `libs/ir/src/ir_optimizer.cpp` (`optimize`, `isInlinable`, `spliceInlinedCall`) |
| Heurística `switch` | `libs/ir/src/ir_builder.cpp:1731-1824` |
| Asignación de registros | `libs/codegen/src/value_placement.cpp` |
| Peepholes y ABI | `libs/codegen/src/codegen.cpp`; `codegen.h` |
| Flags de optimización | `libs/support/include/ceresc/support/optimization.h` |
| Pipeline y subprocesos | `libs/driver/src/driver.cpp` |
| Diagnósticos | `libs/support/include/ceresc/support/diagnostic_id.h`; `docs/11-Diagnostics.md` |
| ISA | `…\core\include\ceres\core\isa\opcodes.h` |
| Registros/flags | `…\isa\registers.h` |
| VM (`call`/`ret`/`enter`/`pushm`) | `…\vm\include\ceres\vm\execution_engine.h` |
| Debug info (CeresASM) | `D:\Projects\CeresASM\docs\21-Debug-Information.md` |
| Convención de llamada | `D:\Projects\CeresASM\docs\24-Calling-Convention.md` |
| MMIO | `D:\Projects\CeresASM\docs\07-IO-Devices-and-Ports.md` |
| Paging | `D:\Projects\CeresASM\docs\27-Virtual-Memory-and-Paging.md` |
| Roadmap Ceres | `D:\Projects\CeresASM\docs\28-Roadmap-Educativo-y-Retro.md` |
| **STDLIB**: construcción/uso | `Makefile`; `tools/mklib.ps1`; `tools/common.ps1`; `tools/runtests.ps1` |
| **STDLIB**: cabeceras | `include/*.h`, `include/ceres/*.h`, `include/ceres/ds/*.h` |
| **STDLIB**: CASM de carencias | `asm/bits.casm`; `asm/math_ops.casm`; `asm/sys.casm`; `asm/setjmp.casm`; `asm/memory.casm`; `asm/string_fast.casm`; `asm/optional/fault.casm` |
| **STDLIB**: piezas clave | `src/malloc.c`; `src/format.c`; `src/exit.c`; `src/fs.c`; `src/math.c`; `src/ceres/fault.c` |
| **STDLIB**: contrato con el compilador | `include/stdarg.h`; `include/stddef.h`; `include/ceres/config.h`; `include/ceres/sys.h`; `include/ceres/ns64.h`; `include/ceres/bits.h`; `include/ceres/atomic.h` |

*Fin del informe.*

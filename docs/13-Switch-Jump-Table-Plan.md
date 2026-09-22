# Plan: tabla de saltos para `switch` en Ceres-C

> Estado: **implementado** (propuestas A y B). La propuesta C (opcode fusionado en la
> VM) sigue sin implementar.
>
> Objetivo: bajar `switch` a algo mejor que una cadena de comparaciones cuando el
> conjunto de `case` es denso, aprovechando lo que la ISA de Ceres **ya** ofrece
> (`LDRX` + `JPR` + relocalización `Word32`), y evaluar dos alternativas de
> implementación más una extensión de ISA opcional.
>
> Implementación: nuevo término IR `IrOpcode::TableJump` (ir_instr.h), la heurística y
> la emisión en `IrBuilder::emitSwitchDispatch`/`emitSwitchTree` (ir_builder.cpp), la
> tabla `.rodata` en `CodeGen::emitJumpTables` (codegen.cpp), y el flag
> `-fjump-tables`/`-fno-jump-tables` (support/optimization.h). Se mantiene el camino
> simple: `-O0` y `-fno-jump-tables` conservan la cadena.
>
> Fuentes de la VM leídas del código, no de la documentación:
> `D:\Projects\CeresASM\Ceres\libs\vm\include\ceres\vm\execution_engine.h`,
> `...\libs\core\include\ceres\core\isa\opcodes.h`,
> `...\libs\asm\src\binary_emitter.cpp`, `...\libs\asm\include\ceres\asm\relocation.h`.

---

## 0. Resumen (TL;DR)

- Sí es posible, y **no hace falta tocar la VM**: la ISA ya tiene `JPR` (salto
  indirecto por registro), `LDRX` (carga indexada `[rs + rt]`) y el ensamblador ya
  emite relocalizaciones `Word32` para tablas de punteros en `.rodata`.
- Se validó **de punta a punta sobre la VM real**: una tabla `u32[4]` en `@rodata`
  con direcciones de etiquetas, despacho con `subi`/`cmpi`/`jae` + `la`/`shl`/`ldr [base+idx]`/`jp reg`,
  y `ceres run` devolvió el status correcto para índices 0, 3, 9 y -1 (rango y
  fuera de rango por ambos lados). Detalle en §3.
- El despacho pasa de coste **O(N)** en instrucciones (cadena de `ifXX`) a
  **O(1)** (~8 palabras), a cambio de `4·(hi-lo+1)` bytes en `.rodata`.
- Se proponen dos implementaciones puramente de compilador (**A** tabla absoluta,
  **B** árbol binario) y una extensión de ISA opcional (**C** opcode fusionado),
  más una heurística de selección y un plan de verificación.

---

## 1. Cómo compila hoy Ceres-C un `switch`

### 1.1 En el IR

`IrBuilder::visit(ast::SwitchStmt&)` (`libs/ir/src/ir_builder.cpp:1725`) recoge los
casos con `collectSwitchCases()` (`libs/ir/src/ir_builder.cpp:1816`) y emite, en
orden de fuente, **un `Cmp` + `CondJump` por valor**:

```cpp
// libs/ir/src/ir_builder.cpp:1736-1746
// Dispatch chain: one Cmp+CondJump per case value, in source order, ...
for (const auto& [value, block] : cases)
{
    IrValue constValue = emitConstInt(loc, value);
    BasicBlock& nextTest = _currentFunction->createBlock();
    emitVoid(loc, IrCondJumpPayload{ IrCmpPredicate::Eq, false, false, condValue, constValue, block, &nextTest });
    _currentBlock = &nextTest;
}
emitVoid(loc, IrJumpPayload{ defaultBlock ? defaultBlock : &exitBlock });
```

El IR resultante de `dayName()` (ejemplo `examples/09_switch_goto.c`) es una
cadena explícita de `br.eq` con un bloque de test por caso:

```
L0:   %1 = load.word [%0]        ; day
      %2 = const 1
      br.eq %1, %2, L1, L10
L10:  %3 = const 2
      br.eq %1, %3, L2, L11
...
L15:  %8 = const 7
      br.eq %1, %8, L7, L16
```

Esto está **fijado por un test dorado** que documenta la decisión de diseño:

```cpp
// libs/ir/tests/test_ir.cpp:410
TEST(ir, switch_lowers_to_a_comparison_chain_not_a_jump_table)
```

### 1.2 En CASM

`CodeGen::generateInstr` traduce `Jump`/`CondJump`
(`libs/codegen/src/codegen.cpp:1093` y `:1102`) a `jp`/`ifXX`. Salida real de
`ceresc -O1` para `dayName` (7 casos densos 1..7 + `default`):

```casm
global dayName:
.L0:
    ifne r0, 1, .L10
.L1:
    la r3, __ccstr0        ; "monday"
    mov r0, r3
    ret
.L2: ... ret
...
.L10:
    ifeq r0, 2, .L2
.L11:
    ifeq r0, 3, .L3
...
.L15:
    ifeq r0, 7, .L7
    jp .L8                 ; default: "not a day"
```

Cada `ifXX` es un pseudo que expande a **2 palabras** (`cmp`+`jz`) y el `jp` final
a 1. Para 7 casos: ~15 palabras ≈ 60 bytes de `.text`, y de media ~N/2
comparaciones ejecutadas por despacho.

---

## 2. Qué ofrece la ISA/VM (evidencia)

La VM no necesita cambios para una tabla de saltos: todas las piezas existen.

| Pieza | Dónde | Qué nos da |
| --- | --- | --- |
| `JP 0x50` / `JPR 0x51` | `opcodes.h:103-104` | `JPR` = `pc = rs` (absoluto, sin comprobación de alineación: `execution_engine.h:1348`). Es el salto final de la tabla. |
| `LDRX 0xB4` | `opcodes.h:211` | `rd = *(u32*)(rs + rt)` — carga indexada sin `add` extra. |
| `SHL 0x37` / `SHLI` | `opcodes.h:75` | escalar el índice por 4. |
| `SUB 0x15` / `SUBI` | `opcodes.h:37` | normalizar `t = x - lo`. |
| `CMPI` + `JAE` | `opcodes.h:106,144` | chequeo de rango **no firmado** en 2 palabras. |
| `LA` (pseudo) | docs 06 | `la rd, tabla` = `lui`+`ori` (2 palabras). |
| `Word32` relocation | `relocation.h:36-42` | un `let t: u32[N] = [etiq, ...]` en `.rodata` genera una relocalización por entrada; **soporta referencias hacia adelante** y compilación separada. |
| Cambio de sección | `translation_unit.cpp` (`_currentSection`) | se puede emitir `@rodata` intercalado con `@text`. |

Aviso práctico: el ensamblador **no** acepta los mnemonics con sufijo `shli`,
`ldrx`, `ori` escritos así; se usa el mnemonic base y el ensamblador elige la
variante por la forma de los operandos (`shl r1,r1,2`, `ldr r3,[r3+r1]`,
`or r0,r0,1`). Confirmado al ensamblar.

### 2.1 La clave: las entradas de tabla deben ser etiquetas de nivel de fichero

Comprobado empíricamente (`ceres asm`):

```
@rodata
    let tabla: u32[3] = [.L0, .L1, .L2]     // ERROR
```

```
[archivo:2] Expected a value in constant expression, got .
```

Las etiquetas locales `.Lx` (que son las que emite hoy el codegen para los
bloques, `libs/codegen/src/codegen.cpp:1459`) **no son válidas** dentro de un
inicializador de datos. En cambio, etiquetas de nivel de fichero sí:

```
@rodata
    let tabla: u32[3] = [ccase0, ccase1, ccase2]   // OK, ensambla
```

Consecuencia para el diseño: los bloques que la tabla referencia deben llevar
etiquetas de fichero (sin `.`). Como emitir una etiqueta no-local en medio de una
función **resetea el ámbito de las etiquetas locales** (`docs 12`), la regla
limpia es: *una función que reciba tabla de saltos emite TODAS sus etiquetas de
bloque como etiquetas de fichero únicas* (`__ccbb_<func>_<id>`), y los `jp`/`ifXX`
de esa función las usan igual. Las funciones sin tabla conservan `.L` (churn
mínimo en los goldens).

---

## 3. Validación empírica en la VM real

Programa mínimo ensamblado y **ejecutado** con `ceres run`
(`D:\Projects\CeresASM\Ceres\build\gcc\bin\Release\ceres.exe`):

```casm
@rodata
    let tabla: u32[4] = [c0, c1, c2, c3]

@text
global main:
    li   r1, 2          ; índice (simula switch(2))
    li   r2, 4
    cmp  r1, r2
    jae  c_def          ; guarda de rango (no firmada)
    la   r3, tabla
    shl  r1, r1, 2      ; índice * 4
    ldr  r3, [r3 + r1]  ; LDRX: dirección absoluta del cuerpo
    jp   r3             ; JPR
c0: ; ... c1: ; ... c2: ; ... c3: ; ...
c_def:
    li r0, 99
fin:
    shl  r0, r0, 8
    or   r0, r0, 1
    la   r13, 0xFFFF0000
    str  [r13 + 0], r0  ; status en bits 15:8
    halt
```

Resultados (`exit` de `ceres run`):

| Índice | Esperado | Obtenido |
| --- | --- | --- |
| 0 | 10 | 10 |
| 3 | 33 | 33 |
| 9 (fuera por arriba) | 99 (default) | 99 |
| -1 = 0xFFFFFFFF (fuera por abajo) | 99 (default) | 99 |

La comparación no firmada tras `t = x - lo` cubre los dos lados **incluso con
índice negativo**, porque `x < lo` desborda a un valor `>= count`. Esto es la
base correcta del guard de rango para `switch` firmados y no firmados.

---

## 4. Análisis de coste

Notación: `N` = número de `case`; `span = hi - lo + 1`; `densidad = N / span`.

**Cadena actual**
- Texto: `2·N + 1` palabras (`ifXX` = 2, `jp` final = 1).
- Ejecución por despacho: ~`N` comparaciones en el peor caso, ~`N/2` de media;
  ramas predecibles pero serializadas.

**Tabla de saltos** (con `lo`/`count` como inmediatos)

```casm
    subi rT, rX, lo          ; t = x - lo            (1)
    cmpi rT, count           ;                       (1)
    jae  .Ldef               ; guarda de rango       (1)
    la   rB, tabla           ;                       (2)
    shl  rT, rT, 2           ;                       (1)
    ldr  rB, [rB + rT]       ; LDRX                  (1)
    jp   rB                  ; JPR                   (1)
```

- Texto: **~8 palabras constantes** (independiente de `N`).
- Ejecución: **constante**, ~8 instrucciones, una carga de memoria.
- Datos: `4·span` bytes en `.rodata`.
- Break-even en tamaño de texto: `8 + 4·span < 4·(2N+1)` → con `span ≈ N`,
  empieza a compensar alrededor de `N ≈ 5-6`; a partir de ahí gana siempre
  porque el texto deja de crecer.
- Aviso: un `default` ausente hace que fuera de rango se salte al bloque de salida
  (`exitBlock`), igual que hoy.

---

## 5. Propuesta A — Tabla de saltos absoluta (compilador, sin tocar la VM)

Es la propuesta principal. Usa solo lo que ya existe.

### A.1 Decisión en `IrBuilder` (recomendada)

Extender `visit(ast::SwitchStmt&)` para elegir entre cadena y tabla según
heurística (§8). Si construye tabla:

1. Ordenar los casos por valor y calcular `lo`, `hi`, `count`.
2. Crear los bloques de caso **una vez** (ya los crea `collectSwitchCases`).
3. Emitir **un** término nuevo en vez de la cadena:

```
L0:
  %1 = load.word [%0]
  tbl.jmp %1, [1..7] -> {L1,L2,L3,L4,L5,L6,L7}, default L8
L1: ... ret
...
L8: ... ret
```

### A.2 Forma del nuevo término IR

`IrOpcode` es un `enum class : u8` cuyo valor se deriva del `index()` del
`std::variant` (`libs/ir/include/ceresc/ir/ir_instr.h:56` y `:352`), y cada
alternativa tiene un payload propio y un `static_assert` de posición
(`ir_instr.h:315-333`). Añadir un opcode obliga a tocar, además:

- `IrOpcode` + payload + variante + `static_assert` (`variant_size` pasa de 16 a 17).
- `forEachOperand` (`ir_instr.h:402`): debe marcar el discriminante como lectura
  (lo necesitan el conteo de usos y la liveness).
- `successorsOf` (`libs/ir/include/ceresc/ir/ir_function.h:232`): debe devolver
  *todos* los bloques destino + el `default`, para que el CFG sea correcto.
- `IrPrinter` (`libs/ir/src/ir_printer.cpp`).
- El `switch` de terminadores de `ir_function.h:241`.
- `CodeGen::usesFloatBank` (`codegen.cpp:1241`) y cualquier otro `switch` sobre
  `opcode()`.

Payload propuesto (trivialmente destructible, requisito del
`static_assert` de `ir_instr.h:367`, y **sin `std::vector`**):

```cpp
struct IrTableJumpPayload
{
    IrValue discriminant;
    BasicBlock* const* targets;   // array en _arena, tamaño entryCount
    u32 entryCount;               // hi - lo + 1
    i64 low;                      // lo
    BasicBlock* defaultTarget;
};
```

El array de destinos se reserva con `_arena` (ya disponible en `IrBuilder`,
`ir_builder.h`), igual que se internan las etiquetas `.strN`.

### A.3 Emisión CASM y de la tabla

`CodeGen::generateInstr` gana un caso `TableJump` que emite la secuencia de §4.
`CodeGen::generate` (`codegen.cpp:2026`) debe, tras emitir las funciones,
emitir en el bloque `@rodata` existente (`codegen.cpp:2147`) una `let` por tabla:

```casm
@rodata
    let __ccjt_dayName: u32[7] = [__ccjc_dayName_1, __ccjc_dayName_2, ...
                                  ..., __ccjc_dayName_7]
```

- El nombre `__ccjt_*` es de nivel de fichero (no empieza por `.`, igual que el
  mangleo de literales `__ccstrN`, `codegen.cpp` `mangledName`), y la función lo
  referencia con `la`, así que **no** dispara el warning de "símbolo privado no
  usado".
- Las direcciones de las entradas son `Word32` relocations resueltas en el enlace
  (`relocation.h:36`), sin ASLR en este proyecto (base de `.text` fija en `0x400`),
  por lo que la dirección absoluta es estable.
- `JPR` no comprueba alineación (`execution_engine.h:1348`); las entradas son
  direcciones de código alineadas a 4, y el guard de rango garantiza que solo se
  salta a entradas válidas.

### A.4 Alternativa A2 — peephole en `codegen`

En vez de un opcode IR, reconocer en codegen la *forma* de la cadena
(`CondJump` de igualdad contra constantes sucesivas sobre el mismo valor, con
`falseTarget` encadenado y `jp` final al `default`) y reescribirla a tabla. Es
coherente con los peepholes ya existentes (`findFusableCmp`, `findFoldableAddress`).
Ventajas: no toca IR/printer/optimizador ni goldens de IR. Inconvenientes: frágil
(depende de que el optimizador no haya enhebrado/colapsado bloques), y necesita
igual la emisión de datos en `.rodata`. **Se descarta como principal**, se deja
como plan B de bajo riesgo.

### A.5 Cambios por fichero

| Fichero | Cambio |
| --- | --- |
| `libs/ir/include/ceresc/ir/ir_instr.h` | `IrOpcode::TableJump`, payload, variante, `static_assert`s, `forEachOperand`. |
| `libs/ir/include/ceresc/ir/ir_function.h` | `successorsOf`, terminadores. |
| `libs/ir/src/ir_builder.cpp` | `visit(SwitchStmt&)`: heurística + emisión del término y del array en `_arena`. |
| `libs/ir/src/ir_printer.cpp` | imprimir `tbl.jmp`. |
| `libs/ir/src/ir_optimizer.cpp` | ver §10 (threading, unreachable, inline). |
| `libs/codegen/*` | caso `TableJump`, etiquetas de bloque de fichero, emisión de la tabla. |
| `libs/support/include/ceresc/support/optimization.h` | flag `jump-tables` (§8). |
| `docs/03-IR-to-CASM.md`, `docs/06-Known-Limitations.md`, `examples/09_switch_goto.c` | documentar. |
| tests | §11. |

---

## 6. Propuesta B — Árbol de decisión binario (compilador, sin tabla)

Cuando el conjunto es **disperso o de rango enorme**, una tabla es inaceptable
(rellenar huecos cuesta `4·span`). La alternativa es ordenar los casos y emitir
un **árbol binario de comparaciones** sobre el valor: profundidad `⌈log2 N⌉`, y
en cada nodo una comparación de orden (`jls`/`jgr` con signo) que envía a la
mitad correspondiente, más una comparación de igualdad o un nodo hoja por caso.

- Coste de ejecución: `O(log N)` comparaciones (vs `O(N)` de la cadena).
- Coste de texto: `~2·N` palabras (una por nodo interno/hoja), sin `.rodata`.
- No necesita relocalizaciones, ni etiquetas de fichero para bloques, ni emisión
  de datos; **no cambia la VM** y es incremental sobre el IR actual.
- Ideal para valores como `case 0: case 1000: case 500000:` o enums dispersos.

Implementación: en `visit(SwitchStmt&)`, ordenar `cases` por valor y construir
recursivamente bloques de test (reutilizando `emitConstInt` + `CondJump` sobre
`IrCmpPredicate::Lt/Le/Eq`), con los bloques de caso ya existentes como hojas.
Reutiliza toda la infraestructura de hoy (sin opcode nuevo).

---

## 7. Propuesta C — Extensión de ISA: opcode fusionado (opcional, futuro)

Solo si A/B no bastan. La VM tiene hueco libre en `opcodes.h:258`
(`0x7A-0x7F`, `0x8C-0x8F`, `0xD7-0xFF`). Un opcode que fusione
guard + carga indexada + salto reduciría el despacho de ~8 a ~4 palabras y
evitaría gastar dos registros temporales:

```
SWJ 0x7A  [rd, rs, rt]
    si (u32)rs >= (u32)rt  -> advancePC()        ; fuera de rango: cae al default
    si no                   -> pc = mem32[rd + rs*4]   ; sin advancePC
```

Uso: `subi rT, rX, lo` / `la rB, tabla` / `li rC, count` / `swj rT, rB, rC` /
`jp .Ldef`. Coste: hay que tocar `opcodes.h`, `execution_engine.h` (handler +
registro en la tabla de dispatch), `instruction_info.cpp`, el desensamblador,
la documentación y los tests de la VM (repo `CeresASM`, cambios
multirrepositorio). Beneficio marginal frente a A si el cuello de botella no es
el despacho. **Recomendación: no ahora.**

---

## 8. Selección y optimizaciones transversales

### 8.1 Heurística de selección (borrador)

```text
si N < 3                         -> cadena (hoy)
span = hi - lo + 1
si span > 2^31                   -> cadena/árbol (evita desbordes de lo)
densidad = N / span
si N >= 6 y densidad >= 0.5 y 4*span <= 1024
                                 -> tabla (Propuesta A)
si N >= 6                        -> árbol (Propuesta B)
si no                            -> cadena (hoy)
```

Ajustes: el umbral `4*span <= 1024` limita el `.rodata` (≤256 entradas). Los
valores concretos deben medirse (§11).

### 8.2 Optimizaciones

- **Relleno de huecos con `default`**: un `case` ausente dentro de `[lo,hi]`
  se rellena con el bloque `default` (o `exit`), lo que evita un segundo chequeo.
- **Dedup / jump-threading de entradas**: `case 1: case 2: case 3:` produce hoy
  tres bloques que caen en cascada; el threading existente
  (`ir_optimizer.cpp`) debe propagar a las entradas de la tabla, de modo que las
  tres apunten al mismo bloque final y la tabla se comprima.
- **Rango ajustado**: usar `min/max` reales de los valores para fijar `lo`/`hi`,
  no los declarados.
- **Firmado/no firmado**: `sub` en complemento a dos + `cmp`/`jae` no firmado es
  correcto para ambos si `span <= 2^31` (verificado en §3). Guardar la condición.
- **Registros**: el despacho necesita 1 temporal para el índice y 1 para la base;
  debe reservarlos `ValuePlacement` como ya hace con el destino de un `call`
  indirecto, y el discriminante debe considerarse vivo en la terminación.
- **`-f` y `-O0`**: añadir `bool jumpTables` a `OptimizationOptions` y a la tabla
  `optimizationFlags()` (`optimization.h:42-106`) con nombre `jump-tables`,
  **activada en O1** y desactivada en `-O0` por construcción (la tabla de flags
  recorre todos los campos). Así `-O0` conserva la cadena simple — coherente con
  la filosofía de bisecabilidad del proyecto.
- **Interacción con el optimizador**: `unreachable-block-elimination` y
  `jump-threading` deben reconocer el nuevo terminador; el inliner ya rechaza
  funciones con `Jump`/`CondJump` por el remapeo de `BasicBlock*`
  (`ir_optimizer.cpp`, `remapInstr`), y debe rechazar también `TableJump` (mismo
  motivo) — comportamiento seguro, no un fallo.

---

## 9. Plan de verificación

1. **IR**: test que fije la forma `tbl.jmp` para un `switch` denso, y que un
   `switch` disperso siga dando cadena/árbol. Sustituir/actualizar
   `test_ir.cpp:410`.
2. **Codegen dorado**: `dayName` en `-O1` (tabla) vs `-O0` (cadena), y el caso
   `-fno-jump-tables`.
3. **E2E**: `examples/09_switch_goto.c` incluye `dayName` (1..7 denso),
   `weight` (1,2,3,9 disperso) y `countDigits` (48..57 denso) — cubre los tres
   regímenes. Verificar salida idéntica a la actual.
4. **Bordes**: `case` negativo (p.ej. `-3..3`), `case 0`, sin `default`, enum,
   `char`, `switch` anidado, `break` dentro de bucle, `case` duplicado (ya lo
   rechaza sema), rango `INT_MIN..INT_MAX` (no debe generar tabla).
5. **Rendimiento**: micro-benchmark de un `switch` denso de 32 casos contra la
   cadena, contando instrucciones ejecutadas (el Profiler ya expone
   `executionCountsData`, `execution_engine.cpp:132`).
6. **Regresión**: `detect_changes()` antes de commit; los goldens de CASM que
   citan `.L` solo cambian en funciones con tabla.

---

## 10. Impacto y riesgos

- **Blast radius de símbolos** (GitNexus): `collectSwitchCases` y el `visit` de
  `SwitchStmt` tienen 1 dependiente directo cada uno (bajo). `IrOpcode` no tiene
  dependientes directos indexados, pero su `enum` es consultado por `switch` en
  IR/printer/codegen/optimizer; el compilador obliga a tratarlos todos
  (exhaustividad). Riesgo global: **MEDIO** por amplitud, no por profundidad.
- **Riesgo principal**: el remapeo de bloques del optimizador. Cualquier pass que
  reescriba `BasicBlock*` de `Jump`/`CondJump` debe reescribir también
  `TableJump::targets[]` y `defaultTarget`, o la tabla saltará a bloques
  obsoletos. Mitigación: test dedicado con `-O2` sobre un `switch` que caiga en
  un `case` con `jump-threading`/`unreachable-blocks`.
- **Riesgo de etiquetas**: la regla "función con tabla → todas sus etiquetas de
  bloque de nivel de fichero" debe respetar colisiones. `__ccbb_<func>_<id>` es
  único por TU (nombres de función únicos + id por función), y las etiquetas de
  fichero no cruzan TU, así que no hay colisión.
- **Compatibilidad**: ningún `.cres` existente usa el nuevo término IR (es
  interno); no hay cambios de formato binario ni de ABI.

---

## 11. Resumen comparativo

| Criterio | Cadena (hoy) | A. Tabla | B. Árbol | C. Opcode VM |
| --- | --- | --- | --- | --- |
| Complejidad de despacho | O(N) | O(1) | O(log N) | O(1) |
| Texto | 2N+1 | ~8 const | ~2N | ~4 const |
| Datos `.rodata` | 0 | 4·span | 0 | 4·span |
| Cambios en la VM | 0 | 0 | 0 | Sí (ISA) |
| Nuevo opcode IR | — | Sí | No | Sí |
| Denso (N≥6) | malo | **mejor** | bueno | mejor (marginal) |
| Disperso / rango enorme | regular | inviable | **mejor** | inviable |
| Riesgo de implementación | — | medio | bajo | alto (multirrepo) |

**Recomendación**: implementar **A** (opcode IR `TableJump` + emisión de tabla en
`.rodata`) y **B** como *fallback* para switches dispersos, ambas tras el flag
`jump-tables` (ON en O1, OFF en O0). Dejar **C** como línea futura solo si la
medición demuestra que el despacho es el cuello de botella.

---

## Anexo — Fragmentos citados

- Cadena actual: `libs/ir/src/ir_builder.cpp:1736-1746`.
- Golden de IR: `libs/ir/tests/test_ir.cpp:410`.
- Opcode/payload/variante: `libs/ir/include/ceresc/ir/ir_instr.h:56`, `:304`, `:315`.
- Sucesores de bloque: `libs/ir/include/ceresc/ir/ir_function.h:232`.
- Emisión `Jump`/`CondJump`: `libs/codegen/src/codegen.cpp:1093`, `:1102`.
- Etiqueta de bloque actual: `libs/codegen/src/codegen.cpp:1459`.
- Opciones de optimización: `libs/support/include/ceresc/support/optimization.h:42-106`.
- ISA: `JPR` `opcodes.h:104`; `LDRX` `opcodes.h:211`; huecos `opcodes.h:258`.
- VM `JPR`: `execution_engine.h:1348`.
- Reloc `Word32`: `libs/asm/include/ceres/asm/relocation.h:36`.
- Tablas de punteros en datos: `docs/11-Data-Types-and-Literals.md` §"An initializer can be an address".
- Ámbito de etiquetas locales: `docs/12-Labels-and-Symbols.md`.

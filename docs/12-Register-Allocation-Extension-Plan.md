# Plan: ampliar los registros utilizables por Ceres-C

> Estado: propuesta de diseño. No hay cambios aplicados todavía.
>
> Objetivo: ampliar el abanico de registros que Ceres-C asigna a parámetros,
> variables y temporales, apoyándose en lo que la VM **realmente** hace en
> `call`/`ret`/`enter`/`leave`, no en lo que dice la documentación.

---

## 1. Qué hace de verdad la VM (leído del código, no de la documentación)

Fuente: `D:\Projects\CeresASM\Ceres\libs\vm\include\ceres\vm\execution_engine.h`
y `...\vm\src\execution_engine.cpp`.

### 1.1 `call` / `callr`

```
CALL  (execution_engine.h:1396-1403)
    push<u32>(_pc + 4);          // apila SOLO la dirección de retorno
    _pc += simm24;               // salta

CALLR (execution_engine.h:1404-1409)
    push<u32>(_pc + 4);
    _pc = rs;                    // salto indirecto
```

No se salva ni se destruye **ningún** registro de propósito general ni flotante.
No se tocan las flags.

### 1.2 `ret`

```
RET (execution_engine.h:1426-1430)
    pop<u32>() -> _pc            // desapila la dirección de retorno
```

Solo desapila. No restaura registros.

### 1.3 `enter` / `leave`

```
ENTER imm16 (execution_engine.h:1488-1501)
    push<u32>(fp());             // salva el fp del llamador
    fp = sp;                     // fp apunta al fp salvado
    sp -= frameSize;             // abre el frame (pila crece hacia abajo)

LEAVE (execution_engine.h:1502-1510)
    sp = fp;                     // deshace el frame
    pop<u32>() -> fp             // restaura el fp
```

Solo tocan `fp` (r14) y `sp` (r15). Nada más.

### 1.4 `push`/`pop` y la dirección de la pila

```
push<T> (execution_engine.h:382-406):  sp -= sizeof(T); write(sp, value)
pop<T>  (execution_engine.h:410-436):  value = read(sp); sp += sizeof(T)
```

La pila crece hacia abajo desde `memory.size() - SystemStackSize`
(`execution_engine.cpp:12`). Hay un solo puntero de pila: la "pila de llamadas" es
la misma pila que todo lo demás.

### 1.5 `pushm`/`popm`

```
PUSHM mask (execution_engine.h:1439-1463): por cada bit del mask (de r15 a r0) hace push
POPM  mask (execution_engine.h:1464-1485): por cada bit del mask (de r0 a r15) hace pop
```

`mask` es `imm16`, un bit por registro. **Una sola instrucción** salva/restaura
cualquier subconjunto de los 16 registros enteros. No existe equivalente con máscara
para flotantes: solo `fpush`/`fpop` (uno por registro).

### 1.6 `bl`/`blr`

```
BL  (execution_engine.h:1412-1418): rd = pc+4; pc += simm20
BLR (execution_engine.h:1419-1425): rd = pc+4; pc = rs
```

Ponen la dirección de retorno en **un registro** sin tocar la pila.

### 1.7 Interrupciones

```
triggerInterrupt (execution_engine.cpp:30-96):
    si _interruptDepth == 0:  sp = memory.size()   // cambia a la PILA DE SISTEMA
    push<u32>(flags); push<u32>(pc);               // solo flags + pc
IRET (execution_engine.h:949-971): pop pc; pop flags; restaura sp
```

La interrupción **no salva ningún registro de propósito general ni flotante** del
programa interrumpido. Corre sobre una pila distinta. El handler debe salvar él
mismo todo lo que toque.

### 1.8 Conclusión central

> **El VM no destruye nada en `call`/`ret`/`enter`/`leave`.** La división
> "caller-saved / callee-saved" (`r0-r7`/`r12`/`f0-f7` frente a `r8-r11`/`f8-f15`)
> es **pura convención del compilador**, no un comportamiento del hardware. Las
> flags tampoco las toca el `call`: las pisa la aritmética del callee, no la llamada.

Lo único que el hardware reserva de verdad es:

| Registro | Rol real (no negociable) |
|---|---|
| `r13` (`at`) | Scratch del **ensamblador** al expandir `stv` y el `ldv` flotante (`asm/src/instruction_info.cpp:58-65`, `operand.cpp:54-68`) |
| `r14` (`fp`) | Frame pointer (`enter`/`leave`) |
| `r15` (`sp`) | Stack pointer |

`r0-r12` son contiguos y **todos utilizables** (lo dice el propio comentario del
ensamblador). Los 16 `f0-f15` son utilizables (no hay `f-at`/`f-fp`/`f-sp`).

---

## 2. El fichero de registros real

`ceres/core/include/ceres/core/isa/registers.h:111-147`:

```
r0  .. r12   propósito general (13 registros)
r13 = AT     assembler temporary (scratch del ensamblador)
r14 = FP     frame pointer
r15 = SP     stack pointer

f0 .. f15    flotantes, los 16 de propósito general
```

### Uso actual de Ceres-C (`libs/codegen/src/value_placement.cpp:20-32`)

| Banco | Con llamadas | Sin llamadas (hoja) |
|---|---|---|
| Entero asignable | `{6,7,12}` | `{6,7,12,0,1,2,3}` |
| Flotante asignable | `{6,7}` | `{6,7,0,1,2,3}` |
| Scratch | `r4`/`r5`, `f4`/`f5` (fijos, nunca asignados) | |
| Nunca usados | `r8-r11`, `f8-f15`, `r13` | |

Convención de argumentos (seguida, no inventada): 4 args por banco en `r0-r3`/`f0-f3`
contados por separado, retorno en `r0`/`f0`, resto a la pila.

---

## 3. El hueco real (dónde hay registros sin usar)

1. **`r8-r11` (4 enteros) y `f8-f15` (8 flotantes): los callee-saved.** Hoy no se
   asignan *nunca*. El motivo escrito en `value_placement.h:41-43` es que usarlos
   "obligaría a salvarlos/restaurarlos alrededor de cada función". Con `pushm`/`popm`
   eso cuesta **una instrucción** por lado (enteros), no un drama.

2. **El caso del usuario — `r2`/`r3` libres cuando solo se usan `r0`/`r1`:** ya está
   explotado, pero **solo en funciones hoja**. En una función que llama a otra, los
   pools caen a `{6,7,12}` y `r0-r3`/`f0-f3` quedan vetados *por completo*, aunque una
   variable concreta no viva a través de ningún `call`. Ahí hay ineficiencia.

3. **`r13` (`at`):** libre mientras Ceres-C no emita `stv` ni `ldv` flotante (hoy
   no lo hace: usa `la`+`ldr`/`str`). Usable, pero con aviso (ver §5).

4. **`r14` (`fp`) en funciones no variádicas sin frame:** técnicamente libre (el
   acceso a slots es `[sp + slotN]`), pero `enter`/`leave` lo salvan/restauran;
   reaprovecharlo obliga a gestión manual de frame. Valor marginal.

### Implicación para el ejemplo del usuario

"Parametros en `r0-r3`, solo se usan `r0` y `r1`" → en hoja ya se usan `r2`/`r3`.
La ganancia de verdad está en **darle a las variables un hogar en registro a través
de `call`s**, que hoy es imposible (una variable solo recibe registro en funciones
que no llaman a nada, `value_placement.cpp:291`).

---

## 4. Opciones analizadas

### Opción A — Usar los callee-saved (`r8-r11`, `f8-f15`) con save/restore (RECOMENDADA)

- Nuevo pool *callee-saved* para **locals** (y temporales con rango de vida que
  cruza calls), además del pool *caller-saved* actual para rangos sin calls.
- La función que los use emite `pushm mask`/`popm mask` (enteros) y `fpush`/`fpop`
  (flotantes) alrededor de su cuerpo.
- **No rompe la interoperación**: la convención ya dice que el callee debe preservar
  `r8-r11`/`f8-f15`. Ceres-C es solo el último en empezar a *usarlos y preservarlos*.
  El `extern`/`.decls` con CASM escrito a mano sigue valiendo.
- Coste: 2 instrucciones por función que los use (una vez), cambio localizado.

### Opción B — Expandir el pool caller-saved más allá del ABI

- No hay dónde: el caller-saved ya es `r0-r7`+`r12` = todo `r0-r12` salvo los
  callee-saved. "Ampliar argumentos a `r4-r7`" rompería la convención documentada de
  CeresASM de la que depende el CASM a mano y los `.decls`.
- **Descartada** como cambio de convención; ver Opción D para el refinamiento interno.

### Opción C — Reclamar `r13` (`at`)

- Añadirlo al pool entero. Riesgo: si algún día se emite `stv`/`ldv` flotante (o
  código a mano importado lo usa), `at` se destruye silenciosamente.
- **Diferida**: ganancia pequeña (1 registro) frente a riesgo de semántica sutil.

### Opción D — Refinar caller-saved: derramar solo *alrededor del call concreto*

- Hoy la regla es binaria: "función con calls" ⇒ `r0-r3` vetados. Un temporal que
  vive a través de un único `call` podría salvarse justo antes y recargarse justo
  después (o, mejor, ir a un callee-saved de la Opción A).
- Más trabajo (análisis de presión por call), pero ortogonal y compatible.

### Opción E — `bl`/`blr` para hojas con retorno en registro

- No añade registros; solo evita el `push`/`pop` del retorno. Curiosidad, no prioridad.

### Resumen de trade-offs

| | Ganancia | Coste | Riesgo interop | Complejidad |
|---|---|---|---|---|
| A (callee-saved) | ALTA (locals a través de calls) | bajo (`pushm`/`fpush`) | nulo | media |
| D (spill por call) | media | bajo | nulo | media-alta |
| C (`at`) | baja | bajo | sutil (`stv`) | baja |
| B | — | — | alto | — |

---

## 5. Plan propuesto (por fases, sin tocar el VM)

**Premisa:** todo esto es trabajo del compilador. El VM no cambia.

### Fase 1 — Habilitar los callee-saved (núcleo)

1. `value_placement.cpp`: añadir dos pools nuevos
   `allocatableCalleeSavedInt = {8,9,10,11}` y
   `allocatableCalleeSavedFloat = {8,...,15}`.
2. En `assignLocals` (hoy guardada por `!hasCalls`): permitir que un local reciba un
   registro callee-saved **aunque haya calls**, manteniendo las exclusiones de
   corrección (escapado, `volatile`, ancho > word, param en pila). Un local con rango
   de vida que cruza calls solo puede ir a callee-saved, nunca a caller-saved.
3. Registrar qué callee-saved usó la función (máscara entera + lista flotante).
4. `codegen.cpp`: en el prólogo/epílogo, si la máscara es no vacía, emitir
   `pushm mask` (antes de `enter`) y `popm mask` (después de `leave`); para flotantes
   `fpush fN`/`fpop fN`. Orden verificado en §1.3/1.5: `pushm` → `enter` / `leave` →
   `popm` es correcto por construcción.
5. Mantener las dos pasadas `register`/normal de `assignLocals` para que el keyword
   siga reordenando preferencias sobre el pool nuevo.

### Fase 2 — Interrupciones: ampliar la máscara de salvado

- `codegen.h:377-378`: `kInterruptSaveMask` pasa de `0x10FF` (`r0-r7`+`r12`) a
  `0x1FFF` (`r0-r12`), y `kInterruptSavedFloatCount` de 8 a 16. Obligatorio en cuanto
  `r8-r11`/`f8-f15` son asignables: un handler puede interrumpir a una función que
  tiene valores vivos en ellos.
- Coste: enteros sigue siendo **un** `pushm`; flotantes solo si el handler los usa
  (`_interruptSavesFloats`).

### Fase 3 — Refinamiento caller-saved (Opción D, opcional)

- Sustituir el veto binario `hasCalls` por "el rango de vida concreto no cruza un
  `call`" también para `r0-r3`/`f0-f3`; o derramar selectivamente alrededor del call
  puntual. Reutiliza la liveness ya calculada (`computeLiveness`).

### Fase 4 — `r13` (`at`) bajo bandera (opcional, con aviso)

- Añadir `r13` al pool solo bajo una opción explícita de línea de comandos, con un
  diagnóstico que advierta del peligro `stv`/`ldv` flotante. Mantenerlo fuera por
  defecto.

---

## 6. Puntos de contacto del código

| Cambio | Archivo / símbolo |
|---|---|
| Pools de asignación | `libs/codegen/src/value_placement.cpp` `allocatableIntRegisters` / `allocatableFloatRegisters` (y nuevas `...CalleeSaved...`) |
| Asignación de locals/temporales | `value_placement.cpp` `assignLocals`, bloque `if (options.registerAllocation)` |
| Prólogo/epílogo de salvado | `libs/codegen/src/codegen.cpp` (emisión de `enter`/`leave` + `pushm`/`popm`/`fpush`/`fpop`) |
| Máscara de interrupción | `libs/codegen/include/ceresc/codegen/codegen.h` `kInterruptSaveMask`, `kInterruptSavedFloatCount` |
| Documentación de convención | `docs/03-IR-to-CASM.md` §"Registers and frames", `docs/07-CASM-Interop.md` §"Caller-saved registers" |

---

## 7. Verificación

- Tests dorados: el camino `-O0` (sin regalloc) queda intacto por diseño; los `-O2`
  de `libs/codegen/tests/test_codegen.cpp` que pinan registros concretos necesitarán
  actualizarse a la nueva asignación.
- `detect_changes()` antes de commit, como exige `AGENTS.md`, y `impact` sobre
  `ValuePlacement` / `CodeGen::generateFunction` (símbolos de riesgo alto).
- Pruebas de extremo: función con calls que usa 1/4/8 callee-saved; handler que usa
  flotantes; `register` sobre un local en función con calls; interop con el
  `examples/interop` existente (el CASM a mano ya preserva callee-saved por convención).

---

## 8. Por qué esto responde al ejemplo del usuario

"`r0-r3` de parámetros con solo `r0`/`r1` usados" ya se aprovecha en hojas. Lo que de
verdad estaba bloqueado — y que este plan destapa — es que **ninguna variable puede
vivir en registro en una función que llama a otra**, porque los únicos registros
"estables" (`r8-r11`/`f8-f15`) se mantenían sin usar. Fase 1 los pone a trabajar:
un local que hoy se derrama a un slot del frame podrá quedarse en `r8` (o `f8`)
durante toda la función, con un `pushm`/`popm` de coste fijo, sin romper la
convención con el CASM a mano.

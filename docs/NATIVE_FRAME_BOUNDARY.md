# AC6: contexto y límites del render nativo — 02-11-2026

## Alcance

Análisis **estático, sin ejecutar el juego ni SteamVR**, del PE identificado
por SHA-256 `e003771e5548bca91882c1d7dfe2df3de97cee05c2c989d0c0e34b036ea610d6`.
Las direcciones siguientes son **RVA del ejecutable identificado**. Se revisó
el flujo de instrucciones x64 con Capstone/pefile; los nombres son etiquetas
de investigación, no símbolos ni observaciones de llamadas reales.

Esta inspección continúa la [ruta nativa de vistas](NATIVE_VIEW_ROUTE.md),
el [censo de recursos](NATIVE_VIEW_RESOURCES.md) y la comparación con el
segundo pase de [Elden Ring VR](ELDEN_RING_VR_REFERENCE.md). La existencia
de un segundo pase en Elden Ring no acredita que AC6 admita reentrada.

## Contextos alrededor de la llamada al renderer

| Sitio | Relación observada | Límite de la conclusión |
| --- | --- | --- |
| `0x1292b99` → `0x23a87c0` | Obtiene puntero de contexto, conservado en `rbx` por `0x1292b9e`. | No prueba identidad de frame ni lifetime. |
| `0x1292ba1` | Escribe `1` en `[rax+0x25f0]` (byte). | Un flag activo no es una barrera de frame. |
| `0x1292c05` / `0x1292c07` / `0x1292c0a` | Comprueba manager; pasa `rbx` como segundo argumento a `0x15194d0`. | Llamada condicionada por la rama. |
| `0x1292c12` → `0x23a89b0` | Cierra el primer contexto. | El callee contiene ramas; no prueba final de frame. |
| `0x1292c17` → `0x23a87c0` | Vuelve a obtener contexto; `0x1292c2e` puede llamar a `0x1519240`. | No demuestra que sea el mismo contexto ni un segundo ojo. |
| `0x1292cd9` → `0x23a89b0` | Otro sitio alcanzable de cierre. | Sin prueba de emparejamiento temporal de ambos cierres. |

En `0x23a89b0`, una ruta escribe `0` en `[rbx+0x25f0]` (`0x23a89e3`)
y contiene llamadas a `0x22dbbf0` (`0x23a89f8`) y `0x22dbe60`
(`0x23a8a07`). **El cierre es condicional**; estas instrucciones no se
ejecutan necesariamente en todos los recorridos.

## Estado y operaciones del renderer

| Sitio en `0x15194d0` | Trabajo estático verificado |
| --- | --- |
| `0x15194fe` | Conserva el argumento de contexto (`rsi = rdx`). |
| `0x1519504` / `0x151950d` | Obtiene propietario desde `manager+0x10`; puede invocar el prepaso `0x151d580`. |
| `0x1519519` | Guarda contexto en un campo compartido `+0x30`. |
| `0x1519557` / `0x1519564` / `0x15195a3` | Llama al setter `0x1297340`, recorre vistas vía `0x2466510` y llama al clear `0x1297350`. |
| `0x15195a8` y siguientes | Rama opcional que contiene despachos `0x321bbf0`, `0x237a6f0`, `0x237c430` y otros. |

Los setters incluyen escrituras de `1` (`0x1297340`) y `0`
(`0x1297350`) en `[rcx+0xcc]` (dword). La lista de vistas procede de
`[rdi+8]`; el argumento de contexto se obtiene de `[rbx+0x30]`.
Esto no prueba que todos los recorridos ejecuten la limpieza, que el estado
quede separado entre ojos o que un segundo render carezca de efectos.

### Prepaso condicional `0x151d580`

El prepaso lee `owner+0x3c8` (`0x151d5b3`), obtiene un objeto por su vtable
`+0x18` (`0x151d5ce`) y contiene estas operaciones:

- Limpia un byte del objeto obtenido en `+0xc16e` (`0x151d5e7`).
- En una rama modifica contadores del propietario en `owner+0x780`
  (`0x151d6b7`).
- En otra ruta marca `owner+0x790 = 1` (`0x151d705`) y llama a
  `0x231eaf0` (`0x151d700`).

Son **sitios alcanzables en el CFG estático**, no una traza de ejecución
conjunta. No se conoce el backing GPU del objeto devuelto por la llamada
virtual. Estos cambios del propietario refuerzan la necesidad de investigar
los efectos laterales antes de intentar un segundo pase; sus significados
exactos siguen abiertos.

### Continuación: configuración mutable y despacho por cuatro entradas

La inspección de los destinos de las llamadas del prepaso revela dos rutas
adicionales con estado compartido. El CFG del ejecutable identificado confirma
sus instrucciones y saltos entre secciones, **pero no acredita qué ramas se
toman durante un frame ni que las cuatro entradas correspondan a ojos**.

| Condición y sitio | Efecto visible en el código | Límite |
| --- | --- | --- |
| `0x151d5ee`, `owner+0x760` | Condiciona la preparación de un bloque temporal mediante `0x151d340`. | No se conoce el evento que activa el flag. |
| `0x6663ec0` y `0x6663f10` | Incorpora 16 bytes de `owner+0x770` al bloque temporal; evalúa `target+0xc16d`. | El salto fuera de línea es un bloque ejecutable del mismo PE; no es una traza. |
| `0x151d66c` → `0x151cc10` | Bajo la rama correspondiente, copia aproximadamente `0xc0` bytes del bloque temporal a `target+0xc0`. El callee salta a `0x5a6c31e` y termina tras copiar hasta `+0xbc`. | No se ha identificado la semántica de todos los campos; el target es mutable. |
| `0x151d680`/`0x151d685` | Despacha al helper `0x231eaf0` con máscara `0xff` y `r8d=0`. | Solo en la ruta condicionada por `target+0xc16d`. |
| `0x151d6a0..0x151d6d9` | Recorre cuatro contadores `owner+0x780..+0x78c`, los decrementa condicionalmente y forma una máscara de bits. También consulta `owner+0x81c`. | Máscara de cuatro posiciones; no indica cuatro vistas/ojos. |
| `0x151d6f1..0x151d705` | Si hay máscara y target, llama `0x231eaf0` con esa máscara y el byte previo `owner+0x790`; luego marca este byte a `1`. | No se ha demostrado la cadencia de activación ni su relación con el frame. |

`0x231eaf0` comprueba `target+0xc157`, suma `0x3e80` al puntero
y salta a `0x23f44f0`. Este helper descarta una máscara nula, recorre cuatro
slots con referencias en `rcx+0x160+8*i`, consulta bytes de una zona **global**
en `0x4f7229c+i` y, según el flag de entrada y estado por slot, llama a
`0x23f80b0` o marca `rcx+0xf01+0x1a0*i`. El reset `0x23f80b0` escribe
campos en `rcx+0xee0+0x1a0*i`, `+0xed8`, `+0xf01`, `+0xf04`, `+0xfe0` y,
condicionalmente, `rcx+0x14d0`, `+0x15c0` y `+0x15d0`. Los nombres
«slot», «máscara» y «reset» son descripciones de instrucciones, no símbolos
del juego. **La ruta modifica un objeto compartido y consulta estado global**;
repetir el prepaso para un ojo adicional exigiría comprender su ownership.

Además, si `owner+0x70` es no nulo, `0x151d5ae` llama a `0x23891d0`
con un argumento en `owner+0x80`. Esta función consulta un flag global
`0x4d6e371` y, en una rama, el objeto global `0x4f71b70` antes de llamar
a `0x2387580` y, en otra condición, `0x249fbb0`; la alternativa llama
a `0x238a620`. La finalidad de estos despachos aún no está establecida.

## Reproducción, verificación y límites

El verificador [inspect_native_frame_boundary.py](../tools/inspect_native_frame_boundary.py)
comprueba **66/66 puntos**: 11 de contexto, 13 del renderer, 6 de
estado/cierre, 6 del prepaso y 30 de su ruta de despacho/auxiliares. Los grafos de flujo de control seleccionados
se recorrieron sin alcanzar límites del decodificador y todos los anclajes
coincidieron. La herramienta rechaza un PE con SHA distinto y solo lee
secciones ejecutables respaldadas por archivo.

```powershell
.venv-analysis/Scripts/python.exe tools/inspect_native_frame_boundary.py --output evidence/<nombre-nuevo>.json
.venv-analysis/Scripts/python.exe -m unittest tests.test_native_frame_boundary -v
```

Reporte privado más reciente:
`evidence/native-frame-boundary-static-20261102-dispatch-v3.json`.
La versión `...-dispatch-v2.json` conserva 60 anclajes y
`...-dispatch.json` 55; `...-v2.json` conserva los 36 iniciales.
El reporte inicial (`...20261102.json`) conserva 30 puntos previos al
prepaso. Todos permanecen ignorados por Git. Los tests sintéticos validan
instrucciones esperadas, el fallo de un grupo de despacho, el límite del CFG
y el bloqueo ante un fingerprint ajeno.

**Sin demostrar:** ejecución de esas rutas, frame/cámara/proyección/jitter
del dibujo concreto, separación de estado entre ojos, reentrancia de
`0x15194d0`/`0x2466510`, terminación de callbacks, versiones de recursos
D3D12, barreras/fences y correspondencia con `Present`. El decodificador
no atraviesa `call`, no resuelve todos los despachos indirectos y dos puntos
alcanzables pueden pertenecer a ramas mutuamente excluyentes. El reporte
marca `runtime_observed:false`, `safe_to_reenter_proven:false` y
`second_eye_render_proven:false`.

Antes de plantear un segundo ojo hace falta identificar los efectos y el
ciclo del prepaso/cierre, la escritura final de cámara y el conjunto de
targets/historias temporales que utiliza cada vista. Después corresponde
definir un contrato host que rechace pares con destinos mutables compartidos,
y solo con el operador presente considerar una observación natural y acotada
del render. **Ningún segundo render ha sido probado ni autorizado por este análisis.**

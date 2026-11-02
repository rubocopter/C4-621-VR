# Elden Ring VR: segundo pase nativo y condiciones para AC6 (02-11-2026)

## Fuente y alcance

Se inspeccionó el **código** de [IlyaMez/EldenringVR](https://github.com/IlyaMez/EldenringVR),
commit [`590c65e8988bf89aa7ff061e16da708195ee031a`](https://github.com/IlyaMez/EldenringVR/tree/590c65e8988bf89aa7ff061e16da708195ee031a),
en la copia ignorada `reference/eldenringvr-v0.6.0/`. La referencia es GPL-3.0;
estos apuntes describen mecanismos sin incorporar código de ese proyecto.
**Solo análisis de fuentes:** durante esta continuación no se inició ningún
juego ni SteamVR ni se ejecutó código de la referencia.

En Elden Ring, el segundo ojo se genera mediante **otro pase de geometría**
controlado por el motor. Su implementación es un ejemplo relevante de motor
FromSoftware, pero sus grupos de tareas, offsets, campos y garantías de
reentrancia son particulares de Elden Ring y **no se transfieren a AC6**.

Enlaces siguientes: `src/ervr/` en el commit fijado.

## Hallazgos sustentados por el código

1. **Segundo recorrido completo de la cola de tareas.** El hook
   [`Hook_QueueTasks`](https://github.com/IlyaMez/EldenringVR/blob/590c65e8988bf89aa7ff061e16da708195ee031a/src/ervr/frame_probe.cpp#L1660-L1685)
   deja completar primero la llamada original y después ejecuta
   `DoSecondPass(self, a, b)`; dentro de este vuelve a llamar
   [`g_origQueueTasks`](https://github.com/IlyaMez/EldenringVR/blob/590c65e8988bf89aa7ff061e16da708195ee031a/src/ervr/frame_probe.cpp#L1505-L1553).
   Diferencia los pases mediante `g_curPass` y vigila el último `Present` del
   segundo (`g_p2PresentTick`). Que se invoque una función dos veces **no prueba**
   que se haya dibujado y presentado el ojo derecho.

2. **Pase 2 por grupos seleccionados.** El valor por defecto de
   [`vrPass2FullFrame=false`](https://github.com/IlyaMez/EldenringVR/blob/590c65e8988bf89aa7ff061e16da708195ee031a/src/ervr/config.h#L451-L474)
   hace posible un segundo pase acotado a tareas de cámara, dibujo y
   presentación, mediante un conjunto explícito de IDs; existe alternativa
   de *full frame* experimental con más tareas habilitadas. La lógica
   [selecciona grupos y restaura el estado de los que bloqueó](https://github.com/IlyaMez/EldenringVR/blob/590c65e8988bf89aa7ff061e16da708195ee031a/src/ervr/frame_probe.cpp#L1265-L1292)
   y [solo restaura los grupos que continúan bloqueados](https://github.com/IlyaMez/EldenringVR/blob/590c65e8988bf89aa7ff061e16da708195ee031a/src/ervr/frame_probe.cpp#L1505-L1553).
   Es una precaución basada en el flujo observado allí, no demostración
   formal de que el motor no modifique otros campos durante el segundo pase.

3. **Congelar el tiempo requiere más de un `dt`.** El proyecto trata
   [varias copias de pasos, deltas e intervalos ya calculados](https://github.com/IlyaMez/EldenringVR/blob/590c65e8988bf89aa7ff061e16da708195ee031a/src/ervr/frame_probe.cpp#L1293-L1493).
   Dejar `vrPass2Steps=0` como configuración no implica que cada escritura
   interna se aplique: la ruta depende del modo y la disponibilidad de campos.
   El código conserva advertencias acerca de búsquedas heurísticas que
   corrompieron estado. **No adaptar escrituras o escaneos de memoria a AC6**
   sin conocer su propio scheduler y los consumidores de tiempo.

4. **La cámara se reescribe durante el frame.** El proyecto prepara la
   [cámara del segundo ojo](https://github.com/IlyaMez/EldenringVR/blob/590c65e8988bf89aa7ff061e16da708195ee031a/src/ervr/game_er.cpp#L2797-L2845),
   pero además controla un
   [finalizador nativo de cámara](https://github.com/IlyaMez/EldenringVR/blob/590c65e8988bf89aa7ff061e16da708195ee031a/src/ervr/game_er.cpp#L2638-L2660)
   para que las matrices del ojo correcto sobrevivan hasta el dibujado.
   Escribir la matriz únicamente entre dos llamadas a render no basta cuando
   una tarea posterior reconstruye/interpola la cámara. En su modo de tareas
   restringidas hay un replay **acotado a argumentos guardados** del
   finalizador. En AC6 falta identificar la última escritura que consume
   cada vista antes de modificar matrices.

5. **Repetir tareas puede alterar entradas aunque los pasos sean cero.**
   [`combat.cpp`](https://github.com/IlyaMez/EldenringVR/blob/590c65e8988bf89aa7ff061e16da708195ee031a/src/ervr/combat.cpp#L1973-L1981)
   y [su restauración condicional](https://github.com/IlyaMez/EldenringVR/blob/590c65e8988bf89aa7ff061e16da708195ee031a/src/ervr/combat.cpp#L2064-L2097)
   describen módulos de peticiones de acciones evaluados y vaciados de nuevo
   durante el pase 2; se comprueban identidad de jugador/módulo, animación,
   cola y equipamiento antes de reponerlos. AC6 exige un criterio propio para
   evitar doble aplicación de acciones, físicas, timers y transiciones.

6. **Protección de transiciones.** `DoSecondPass` contiene
   [guardas contra carga, warp, muerte, multijugador, cámaras ajenas, fade y
   cambios de flujo](https://github.com/IlyaMez/EldenringVR/blob/590c65e8988bf89aa7ff061e16da708195ee031a/src/ervr/frame_probe.cpp#L1160-L1255).
   Durante desmontaje de escena se omite el segundo pase. Las guardas
   dependen de señales del juego y no son directamente reutilizables.

7. **Emparejar ojos y `Present` es un contrato separado.** En
   [`stereo.cpp`](https://github.com/IlyaMez/EldenringVR/blob/590c65e8988bf89aa7ff061e16da708195ee031a/src/ervr/stereo.cpp#L562-L734)
   se esperan los `Present` de los pases, se correlacionan imágenes con
   cámara/identidad y se puede tolerar un retardo de un frame
   ([`vrPresentLag=1` por defecto](https://github.com/IlyaMez/EldenringVR/blob/590c65e8988bf89aa7ff061e16da708195ee031a/src/ervr/config.h#L656-L671)).
   Si no llega una pareja válida, hay camino de repliegue a cine plano.
   [Los tickets/poses por ojo](https://github.com/IlyaMez/EldenringVR/blob/590c65e8988bf89aa7ff061e16da708195ee031a/src/ervr/stereo.cpp#L238-L263)
   se validan al blit a OpenXR; el lag es una observación de esa ruta,
   **no una configuración recomendada para AC6**.

8. **Riesgos de GPU.** El segundo pase puede reutilizar anillos de upload o
   recursos aún consumidos por la cola. El proyecto experimenta con
   [fences antes y después](https://github.com/IlyaMez/EldenringVR/blob/590c65e8988bf89aa7ff061e16da708195ee031a/src/ervr/frame_probe.cpp#L708-L770),
   pero [`vrGpuSync=0` por defecto](https://github.com/IlyaMez/EldenringVR/blob/590c65e8988bf89aa7ff061e16da708195ee031a/src/ervr/config.h#L656-L671):
   ese mecanismo no solucionó plenamente el parpadeo/negros observados en
   Elden Ring. No considerarlo una sincronización probada para AC6.

## Traslado a nuestro AC6: hipótesis y puertas

La ruta que **sí** tenemos localizada en el PE de AC6 es
[`0x1292c0a` → `0x15194d0` → `0x2466510`](NATIVE_VIEW_ROUTE.md).
El último recorrido ordena vistas, prepara targets e invoca el slot de
render; se observaron llamadas naturales de la vista principal. Por sí solo
**no constituye un límite seguro para repetir un frame**.

### Inspección adicional de la llamada en el PE (02-11-2026)

Con SHA-256 del PE comprobado y **sin ejecutar el juego**, se decodificaron
las instrucciones contiguas a `0x1292c0a` mediante Capstone/pefile:

- `0x1292bfb`: obtiene el global del manager; `0x1292c02` comprueba null;
  `0x1292c07` coloca `rbx` en `rdx` y `0x1292c0a` llama a `0x15194d0`.
- Antes del render está la llamada `0x1292bf6` → `0x13c5590`.
  Inmediatamente después se llama a `0x23a89b0` desde `0x1292c12` y a
  `0x23a87c0` desde `0x1292c17`; más adelante el flujo contiene
  `0x1292c2e` → `0x1519240` (con guardia de puntero null).
- Una búsqueda de operandos directos `E8 rel32` **dentro de secciones
  ejecutables file-backed** encontró un solo candidato hacia `0x15194d0`
  (`0x1292c0a`) y uno hacia `0x2466510` (`0x1519564`). El hallazgo verifica
  esas llamadas concretas; no excluye llamadas indirectas, destinos a través
  de thunks ni confusiones de límites de instrucción al escanear bytes crudos.
- El RVA `0x1292c0a` no aparece cubierto por las entradas de excepciones
  `.pdata` parseadas de este PE. **No se atribuye un principio de función
  o un límite de frame** basándose en ese directorio.

Los vecinos del punto de render ya indican que la operación está intercalada
con otros trabajos del motor. El siguiente análisis estático útil es resolver
qué hacen esos callees, qué estado consume `rbx` y si existe un límite de
render independiente de la actualización; sus nombres y efectos no se
deducen de una secuencia de direcciones.

**Continuación AC6 del 02-11:** el
[censo estático de contexto y prepaso](NATIVE_FRAME_BOUNDARY.md) comprueba
36 sitios del renderer y sus llamadas de contexto: se escriben campos
compartidos y existen mutaciones condicionales del propietario. La semántica
de límite de frame y la reentrancia siguen sin establecerse.

| Pregunta para AC6 | Evidencia existente | Prueba que falta antes de actuar |
| --- | --- | --- |
| ¿Quién invoca el frame y cuándo? | Ruta al renderer de vistas localizada, sin ciclo global completo. | Analizar callers de `0x1292c0a`, secuencia de tareas y si `0x2466510` recibe estado ya finalizado. |
| ¿Cómo se actualiza la cámara por ojo? | Owner/vista/cámara identificados parcialmente. | Última escritura efectiva de matrices, proyección, jitter, viewport y su consumidor GPU antes del draw. |
| ¿Puede repetirse el render sin avanzar la simulación? | Ninguna prueba de reentrancia. | Inventario de efectos de render, acciones, temporizadores, tareas y transiciones; control de un único pase natural antes de modificar. |
| ¿A dónde escriben los dos ojos? | [Censo](NATIVE_VIEW_RESOURCES.md): color/depth principales compartidos con salidas globales. | Salidas independientes o copias seguras por ojo, estados/barreras y backing GPU identificados. |
| ¿Cuándo se pueden liberar vistas? | Construcción y destrucción, 5 wrappers auxiliares y refcounts parcialmente trazados. | Demostrar cierre de callbacks y referencias externas; no fabricar segunda vista con lifetime desconocido. |
| ¿Podremos presentar una pareja válida? | Contratos de procedencia/captura y host DLSS, sin dos renders geométricos. | Frame/eye/pose y sincronización con `Present`, resolución de lag, fallback, historial temporal por ojo y no reutilización de upload. |

**Orden de trabajo offline recomendado:** (1) acabar callbacks/ownership y
reentrancia estática; (2) seguir las llamadas que enmarcan el render natural
y el finalizador de cámara; (3) diseñar criterios de aceptación, contadores
y abortos por transición; (4) implementar controles host que rechacen pares
sin identidad o recursos compartidos. Solo después, con el usuario presente,
plantear un ensayo instrumentado de una variable. Ninguna de las técnicas de
ERVR autoriza aún a invocar el render de AC6 por segunda vez.

Esta referencia amplía [las técnicas externas](EXTERNAL_VR_TECHNIQUES.md),
el [plan](../MODDING_PLAN.md) y la [ruta nativa de vistas](NATIVE_VIEW_ROUTE.md).

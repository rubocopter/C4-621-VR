# Del helper de proyección al contexto de render — 18-10-2026

Continuación desde `95467e8`, después de la [observación v18](FRAME_PROJECTION_PROVENANCE.md#verificación-presencial-v18--pid-11284).
El incremento se probó primero en host. v20 activó correctamente la parada por
segunda cola y no certifica su captura. La repetición autorizada v22, en otro
proceso, verificó 128 muestras con identidad de scope CPU y la procedencia de
tres pares; sigue sin demostrar binding GPU ni proyección por par. Ambas
pasadas se registran debajo. No se ha probado presentación en PSVR2.

## Ruta localizada con los callsites de v18

Binario SHA-256 `e003771e5548bca91882c1d7dfe2df3de97cee05c2c989d0c0e34b036ea610d6`.
REA abrió/verificó esa identidad y cerró la sesión sin análisis global. La
inspección enfocada de instrucciones se hizo directamente con Capstone 5.0.7
y pefile: **no es Evidence de REA ni ejecución observada de esas instrucciones**.
Solo los callsites proceden de las pilas capturadas por el detour v18.

La lista de excepciones parseada automáticamente por pefile contenía solo
19464 entradas, con último EndAddress `0x3cd9a4`; no cubría estos callsites.
La lectura completa del directorio PE de excepciones contiene 312706 entradas
y sí da los rangos siguientes. No se usaron los rangos de ventana aproximados
de la primera exploración como límites de función ni para elegir un hook.

| Ruta estática | Datos seguidos; alcance de la conclusión |
| --- | --- |
| `0x2472a60` → helper `0x23a7790` → `0x2472ae0` | Guarda la perspectiva en objeto destino `+0x90..+0xcf`; el otro argumento procede de `0x23a7ac0` y se guarda en `+0x10..+0x4f`. También forma matrices derivadas. No hay binding D3D12 observado en esa rutina. |
| `0x2641fc0..0x26420c7` | Obtiene la transformación derivada del objeto de vista, llama al helper con jitter habilitado, copia perspectiva a destino `+0x50..+0x8f`, combina matrices y conserva el puntero de vista en `+0x448`. |
| `0x26714b0..0x267153f` | Usa vista en objeto `+0x7b0` y bloques de matrices en `+0x570`, después llama a `0x2641fc0`. Son caminos CPU distintos que pueden compartir valores numéricos. |
| `0x26e17f0..0x26e1da4` | Recibe RCX=escena, RDX=contexto y R8=entrada. Lee la vista desde escena `+0x448`, crea una copia local, llama a los helpers y construye un paquete local de matrices. Después lo entrega a contexto.vtable[1] en `0x26e1d5e`. Antes consulta contexto.vtable[5]. Es el siguiente límite concreto a identificar. |
| `0x2738490` / `0x273bfa0` | Copian matrices al paquete CPU; la segunda guarda cuatro bloques de 16 bytes en destino `+0x930..+0x96f`. Eso no identifica un constant buffer, GPU VA ni draw. |

En `0x26e17f0`, la perspectiva del helper en `0x26e1b42` y la transformación
derivada se pasan a `0x2738490`; otra matriz combinada obtenida por
`0x23a7b00` llega a `0x273bfa0`. La rutina acaba enviando el paquete local a
un método virtual cuyo destino concreto no está establecido estáticamente.
No se asigna a ese paquete el ID de una captura o de un Present.

Salidas privadas del análisis en
`C:/Users/Oni/AppData/Local/ac6-vr-analysis/projection-bindings-20261018/`;
scripts exploratorios en `out/projection-bindings-20261018/`. No se publican
desensamblado/pseudocódigo ni bytes originales del juego. Algunas exploraciones
lineales pueden decodificar padding; las conclusiones anteriores se comprobaron
con los rangos completos y el flujo directo de las rutinas concretas.

## Incremento implementado

`AC6_RENDER_CONTEXT_TRACE=OFF` por defecto; requiere `AC6_FRAME_TRACE=ON`.
El build con ambas opciones añade un noveno hook, en el comienzo identificado
`0x26e17f0`, comprobando prólogo de 16 bytes y las mismas condiciones de host.
Los siete hooks del productor por defecto y la opción anterior de ocho hooks
mantienen su selección. No se cambian la ruta VR ni el protocolo de 120 bytes.

`render_context_trace.hpp` reserva hasta 64 registros inmutables. Cada uno
contiene ordinal de **invocación CPU**, hilo, argumentos, puntero de vtable,
candidatos de slots 1/5 y QPC antes/después. Los punteros se leen con SEH;
no se invocan esos métodos desde la sonda. Los destinos se observan **al entrar**:
`targets_sampled_at:invocation_entry`, `virtual_dispatch_observed:false`.
No se afirma que el método virtual se ejecutó, que la tabla se mantuvo estable
o que dichos destinos sean implementaciones D3D12.

Una identidad TLS está activa únicamente dentro de la llamada al original.
El helper de proyección copia esa identidad **al entrar en su detour** y la
publica con su propia muestra; después no consulta el TLS ni busca un contexto
por proximidad. Un callback anidado obtiene otro ordinal; al salir restaura
el padre. Si una invocación no se observa por pausa, capacidad o puntero
inválido, enmascara temporalmente el padre con identidad cero. Así no hereda
por error su contexto. Los hilos no comparten identidad activa.

El forwarding probado en host conserva los tres argumentos, retorno del
original y excepciones C++. El detour se ejercitó después en v20 dentro del
juego; sus 64 registros completos no demuestran la semántica de todos los
argumentos ni equivalencia de comportamiento en todos los estados.
Los slots reservados por una invocación que lanza excepción
quedan incompletos y no se leen. El worker detiene ambas admisiones, desactiva
hooks y vuelca solo payloads publicados release/acquire, sin esperar callbacks.
Las sondas y trampolines quedan residentes/inactivos. Plazo de ocho segundos
y F12 conservados; no se realizan esperas GPU, asignaciones o I/O en callbacks.

El analizador añade un resultado independiente `cpu_context_scope_*`.
Exige ordinal, hilo e identidad completa coincidentes y anidamiento temporal
coherente dentro de esa invocación; el QPC por sí solo nunca selecciona nada.
Exige QPC enteros positivos int64 e IDs de hilo enteros positivos uint32;
no admite strings, bool ni floats por coerción. Rechaza invocaciones
anteriores/siguientes, duplicados, identidad reciclada,
publicaciones ausentes/incompletas y overflow. Un log antiguo sin estos
eventos dice `not_observed`. No modifica la conclusión de procedencia de
captura ni promueve el resultado a `frame_associated:true` o binding GPU.

## Evidencia host

- Siete CTests con ambas opciones ON y siete con ambas OFF correctos.
  Incluyen los fixtures GPU de captura/fences, saturación y reanudación.
- Pruebas CPU del observador: forwarding, reentrada, enmascaramiento de scopes
  no observados, aislamiento entre hilos, capacidad finita, lecturas inválidas
  y limpieza TLS ante excepción con rechazo de payload incompleto.
- Fixture integrado `test_projection_context`: publica una muestra del helper
  dentro de una invocación y otra fuera; el analizador Python lee el JSONL real
  y confirma exactamente una asociada a **scope CPU**, otra desconocida y
  `cpu_scope_gpu_binding_observed:false`.
- 60 pruebas Python: 59 correctas, una integración GPU opcional omitida.
  Las siete nuevas ejercitan desajustes de ordinal, hilo, argumentos/vtable,
  destinos, intervalos y publicación. RED inicial: función de análisis ausente.
  La revisión detectó además comparación lexicográfica de QPC string y
  equivalencia bool/int para hilos: RED conductual en `typed-scope-red.log`,
  corregido exigiendo tipos/rangos exactos antes de comparar.
- Snapshot parcial determinista: el original simulado no puede terminar hasta
  después de Stop/Write. Se omite su payload reservado y se indica incompleto;
  al liberarlo publica una sola entrada inmutable completa. El worker no espera.
- Logs y builds aislados en `out/render-context-20261018/`. DLL nueva
  `diagnostic/Release/ac6_live_pair_probe_v20.dll`. No se han sobrescrito las
  DLLs v17/v18 ya mapeadas. La ejecución posterior se registra a continuación.

## Observación presencial v20 — PID 11284

Usuario presente y confirmación expresa: «Sí, listo para los 8 segundos con
v20». Se verificó identidad/hash, host cooperativo offline, prólogos originales
y nombre no mapeado antes de cargar la DLL nueva. Sin visor/consumidor ni
seguimiento. El loader terminó en 2 porque el productor detectó otra cola y
publicó Failed: **se conserva el rechazo**. Nueve hooks restaurados, DLL
retenida inactiva y juego respondiendo; no se repitió la carga.

| Observación | Resultado |
| --- | --- |
| Captura | 3 pares y 310 descartes antes del fallo; traza sin overflow ni pares omitidos |
| Colas | DIRECT: 8774 envíos; segunda cola COPY: un envío/lista, no fiable para el ledger; mismo dispositivo |
| Contexto CPU | 64 invocaciones completas, cero inválidas/incompletas/overflow; vtable RVA `0x4098e38`, slot1 `0x2b9d140`, slot5 `0x2b9db10` |
| Helper | 256 muestras, todas fuera del scope seleccionado; cero muestras con identidad CPU resuelta |
| Analizador | `capture_provenance_complete:false`, `producer_failure`; `scoped_projection_candidates:0`, `frame_associated:false` |

`cpu_context_scope_complete:true` en este log solo significa que los registros
de scope son completos y las muestras exteriores están declaradas como tales;
**no** acredita que haya una muestra dentro del contexto. El contador cero es
explícito. No se relaja la parada por COPY aunque esa lista no muestre
transiciones de los recursos seguidos; su ausencia no demuestra que no acceda
a ellos. La causa de la segunda cola sigue sin aislarse.

Evidencia privada: `evidence/render-context-v20-11284-loader.json`,
`evidence/render-context-v20-11284-analysis.json`,
`evidence/render-context-v20-11284-observation.json`, y
`out/render-context-20261018/diagnostic/Release/live_pair_probe_v20_11284.jsonl`.
SHA-256 DLL: `f84ff46582f2d0083387a59dfc1c51ea8555e90c441b2cb52f4742fc482aa75d`.
SHA-256 JSONL: `8f0298c95a7ec0199be45cecf3ef04ec2f114627b937a0b8e165fc2f59d44a61`.

### Destinos concretos y siguiente límite de vida

El análisis estático de los candidatos recogidos establece:

- `0x2b9db10..0x2b9db21` devuelve el puntero en contexto `+8`.
- `0x2b9d140..0x2b9d1c9` comprueba ese puntero; si existe, llama a
  `0x2b9e420`, que escribe el contexto en paquete `+8`. Después obtiene un
  objeto padre en contexto `-0x80`, una interfaz en padre `+0x60` y reenvía el
  paquete a su vtable slot8. Es un adaptador CPU: **no es un binding GPU**.
- La dirección de esa interfaz/destino posterior no está en los registros
  de v20. Una lectura posterior de ocho contextos registrados ya no conservó
  una identidad verificable, por lo que se rechazaron; no se siguieron como
  si fueran objetos vivos de la misma invocación. Evidencia privada en
  `evidence/render-context-v20-11284-passive-targets.json`.

Para seguir el forwarding haría falta recoger esa identidad mientras la
invocación está activa, con comprobación de tipo/layout. Los punteros del
contexto no se pueden reconstruir desde una muestra posterior cercana.

### Cobertura del helper corregida en host

v20 llenó su ventana del helper antes de entrar en el contexto elegido. En el
modo opcional de contexto, el productor nuevo construye
`ProjectionObserver(false,true)`: solo las llamadas con token TLS no cero
consumen slots. Las exteriores se cuentan en `projection_context_window` /
`outside_context_skipped`; el flag `context_only` hace explícito ese filtro.
No filtra por FOV, matriz o coincidencia temporal. Las rutas predeterminadas y
la sonda de proyección sin contexto conservan su admisión anterior.

Fixture nuevo: 266 llamadas exteriores no consumen la capacidad; una llamada
dentro del scope conserva su token y publica una muestra. RED de API ausente
en `scoped-window-red.log`, GREEN de siete CTests en build nuevo
`out/render-context-20261018/scoped` / DLL v22. Antes de la confirmación
presencial, este build no se había cargado. Su posterior ejecución se registra
debajo. Los binarios v17/v18/v20 se conservaron intactos.

## Observación presencial v22 — PID 3228

Después de solicitar un proceso nuevo, el usuario confirmó: «He reiniciado
AC6 y estoy listo en la misión offline». Se ejecutó una sola ventana de ocho
segundos con v22, sin capturas auxiliares, visor, consumidor ni seguimiento.
El loader volvió a verificar ejecutable/hash, módulo cooperativo, ausencia de
EAC, nombre de DLL no mapeado y prólogo de cámara. Terminó con código 0.

| Observación | Resultado |
| --- | --- |
| Captura | 3 pares, 701 descartes por backpressure; una cola DIRECT, sin fallo ni ledger inseguro |
| Traza | 298 listas retenidas, cierre en envío 138; cero overflow y pares omitidos |
| Pares | secuencias 1/2/3, slots 0/1/2, copias en envíos 54/82/110; depth/color en índices 1/2 del mismo envío |
| Contexto CPU | 64 invocaciones completas, 8 escenas y 8 hilos; misma vtable/targets candidatos que v20 |
| Helper | 128 muestras válidas: exactamente dos por invocación, todas con token CPU resuelto; cero inválidas, overflow o publicación incompleta |
| Filtro v22 | 430275 llamadas sin token admitido omitidas antes de consumir capacidad |
| Cierre | nueve hooks restaurados, DLL retenida inactiva, juego respondiendo |
| Analizador | `capture_provenance_complete:true`, `cpu_context_scope_complete:true`, cero errores; `frame_associated:false`, `missing_raster_binding` |

Las dos llamadas por invocación tienen pilas distintas: retorno directo en
RVA `0x26e1b47` y vía `0x23a7b5a` / `0x26e1b7d`. Ambas usan jitter habilitado
y la misma vista dentro de cada scope. Se observaron ocho direcciones de
vista, con un único modelo numérico: FOV Y `1.55334306`, aspecto `1.77777779`,
near `1`, far `500000`, dimensiones `2560x1440`, jitter `(-0.25,0.166666687)`.
No son ocho fotogramas ni ocho cámaras principales demostradas. Las matrices
iguales y las dos llamadas distintas no permiten seleccionar un draw.

La ventana de 64 invocaciones cubre solo 0.2389 ms. Todas las muestras del
helper preceden al primer envío retenido; no se ejercitó un cambio de FOV o
jitter entre fotogramas. El contador de llamadas sin token incluye scopes
posteriores al límite de 64, enmascarados a cero; no significa que todas
pertenezcan a otro camino de render. La captura y el helper tienen identidades
verificadas por separado, **sin enlace entre ambas**. No se relajan las
condiciones tras el rechazo de v20; una cola en v22 tampoco explica la causa
de la segunda cola de aquella pasada.

Evidencia privada:

- `evidence/render-context-v22-3228-loader.json`
- `evidence/render-context-v22-3228-analysis.json`
- `evidence/render-context-v22-3228-observation.json`
- `out/render-context-20261018/scoped/Release/live_pair_probe_v22_3228.jsonl`

SHA-256 DLL: `e6fa7f961f07a9598a822b29d8b9cb311f207d118a6cf7775d22f9e6d0be64b4`.
SHA-256 JSONL: `3dd9fc351602dbd18bd49525abdf50a208b2bdcb05acd3710fc92c694578e4c4`.
La CI de `f4c4163` terminó correcta. No se recompila ni sobrescribe la DLL
v22 ahora mapeada; tampoco las anteriores.

### Procedimiento v22 completado

Después de la parada por segunda cola se requirió una sesión/PID nuevos,
presencia del usuario y confirmación actual de misión cooperativa offline.
Se indicó una ventana de ocho segundos sin visor/consumidor/seguimiento,
girando suavemente con mando. El loader volvió a comprobar ejecutable/hash,
módulos y nombre de DLL nuevo/no mapeado. No se recargaron DLLs anteriores,
no se usó matchmaking ni se inició AC6/SteamVR automáticamente.

Comandos del procedimiento, conservados como referencia; v22 ya está mapeada
inactiva en PID 3228 y **no se debe recargar**:

```powershell
.venv-analysis/Scripts/python.exe -B tools/load_camera_probe.py --pid <PID-NUEVO> --single-player-confirmed --dll out/render-context-20261018/scoped/Release/ac6_live_pair_probe_v22.dll --no-screenshots --capture evidence/context-new-unused.png --output evidence/context-new-loader.json
.venv-analysis/Scripts/python.exe -B tools/analyze_frame_trace.py --log out/render-context-20261018/scoped/Release/live_pair_probe_v22_<PID-NUEVO>.jsonl --output evidence/context-new-analysis.json
```

Éxito: captura normal, nueve hooks restaurados, juego respondiendo; registros
de contexto completos, al menos una muestra del helper con token CPU resuelto
y destinos de vtable no nulos. Conservar JSON del loader, JSONL completo y
análisis, incluso ante rechazo. Si no se obtiene ninguna llamada del helper
dentro del contexto, el resultado será ausencia de cobertura; no se sustituyen
muestras por matrices cercanas ni se declara asociación. Si reaparece otra
cola, detener sin repetir cargas ni eliminar el gate.

La cobertura buscada ya se obtuvo: no hace falta repetir v22. El siguiente
incremento sigue el forwarding del adaptador CPU, recogiendo la
identidad de la interfaz hija y su destino virtual mientras la invocación
está viva, con comprobación de tipo/layout. La instrumentación adicional se
describe debajo. Leer los punteros ahora o repetir la misma sonda no aportaría
ese dato.

Siguen pendientes el rango/versión de bytes usados por GPU, el draw que
escribe el depth capturado, viewport y la correspondencia con TAA/color final.
Esta sonda no supera por sí sola el gate de proyección.

## Forwarding al entrar en el adaptador — incremento v24/v26

`AC6_CONTEXT_FORWARD_TRACE=OFF` por defecto; requiere ambas opciones anteriores
ON. Reutiliza el hook de `0x26e17f0`: sigue habiendo nueve hooks y la ventana de
64 invocaciones/ocho segundos. No altera el protocolo de 120 bytes, fences,
propiedad del ring, frescura, calibración ni presentación VR.

El lector nuevo `context_forwarding_trace.hpp` solo sigue el layout observado
para vtable RVA `0x4098e38`, slot1 `0x2b9d140` y slot5 `0x2b9db10`. Al crear el
hook, antes de habilitarlo, valida los límites PE `.rdata` de solo lectura y
`.text` ejecutable sin escritura y cachea esos rangos. La corrección v26
conserva las secciones `.text` múltiples como rangos separados, sin convertir
sus huecos en código ejecutable. En el callback:

1. Comprueba identidad exacta del adaptador/token y vuelve a leer su vtable y
   slots antes de seguir el layout. Otros tipos se registran como rechazo.
2. Obtiene padre `contexto-0x80`, payload en `contexto+8` e interfaz hija en
   `padre+0x60`. Payload nulo cierra esa lectura, como el guard del adaptador.
3. Lee vtable de la hija y el candidato slot8 (`+0x40`), exigiendo tabla dentro
   de `.rdata` y destino dentro de `.text`. No desreferencia el payload ni
   invoca el destino. Una interfaz cuyo destino esté fuera de esos rangos
   queda rechazada; su semántica no se adivina.

SEH protege las lecturas. Solo se publican todos los punteros o un motivo de
rechazo con payload íntegramente cero. Sin asignaciones, esperas, consultas de
memoria al SO ni I/O en el callback. Los campos quedan dentro de la entrada de
invocación y se publican juntos con el mismo ready release/acquire, después de
volver el original. Una excepción o llamada aún activa no publica su snapshot.
Un original que cambia los enlaces no cambia la copia tomada al entrar.

El JSONL añade `forwarding` anidado, `layout_version:1`,
`sampled_at:invocation_entry`, `dispatch_observed:false`; el resumen declara
`forwarding_enabled:true`. Es versionado diagnóstico, sin nuevos campos en
shared memory. La observación es anterior a la llamada virtual: no demuestra
que ese destino se invoque, que los enlaces no cambien después o que el objeto
hijo tenga semántica GPU. Los punteros no se reutilizan fuera de la invocación.

El analizador informa `context_forwarding_*` aparte de procedencia de captura
y scope del helper. Exige publicación/contexto íntegros, versión y tipos
correctos, parent compatible, token resuelto y ausencia de payload parcial en
rechazos. No transforma un candidato en dispatch ni asociación de fotograma.
Logs anteriores: `not_observed`; el código de salida del CLI sigue expresando
la procedencia de captura, por lo que hay que leer también estos resultados.

### Verificación host

- Ocho CTests correctos con las tres opciones ON y ocho OFF. El test nuevo
  protege layout, adaptador reciclado, enlaces nulos, destino incompatible,
  lectura con slot8 en página inaccesible, copia inmutable antes del original
  y snapshot parcial sin bloquear al escritor. También ejercita las cabeceras
  PE sintéticas: rangos válidos, escritura prohibida y sección fuera de imagen.
- Seis pruebas nuevas del analizador: RED por ausencia del resultado de
  forwarding, GREEN después de implementarlo. Suite completa: 66 pruebas,
  65 correctas y una integración GPU opcional omitida.
- El fixture JSONL del ejecutable C++ se valida con Python; al cambiar su
  contexto por uno reciclado se rechaza. Reanálisis privado de v22/v20 conserva
  respectivamente sus veredictos de captura aceptada/rechazada y declara el
  forwarding no observado. No cambia retrospectivamente su alcance.
- Build nuevo en `out/context-forwarding-20261018/diagnostic/Release/`, DLL
  `ac6_live_pair_probe_v24.dll`. SHA-256:
  `27068ec3bf117051d891fca6b10979ebb0d4b4f92a1cc4a750b0b1260953a6b6`.
  No se había cargado al registrar la primera verificación host. El intento
  posterior y su rechazo se conservan a continuación. Las DLLs anteriores se
  mantienen intactas; build predeterminado aislado con nombre v25.

### Rechazo de preparación v24 — PID 3228

Usuario presente y confirmación expresa: «Sí, listo para los 8 segundos con
v24». El loader verificó identidad/cooperativo, nombre nuevo y prólogo de cámara.
Terminó con código 2: `Render context fingerprint/hook rejected`. No hubo evento
`observing`, captura, ni muestras del helper/contexto. La ruta de fallo se
alcanzó antes de `MH_EnableHook`: el hook de proyección solo se había creado
deshabilitado; los siete hooks D3D12 aún no se habían creado. No se repitió v24.

El registro nativo dice `hooks_restored:false`: en ese punto no se han tomado
los baselines D3D12, y `HookRestored` del contexto no acredita un hook que no
llegó a crearse. Se conserva ese rechazo, sin convertirlo en restauración
verificada de nueve hooks. La comprobación externa **de solo lectura** verificó
por separado los prólogos originales de cámara, proyección y contexto, además
de cabeceras PE vivas iguales a disco. Juego respondiendo; DLL retenida inactiva.

La causa se reprodujo en host: el ejecutable fijado contiene dos `.text`
(`0x1000..0x3703c00` y `0x53cb000..0x6c2fc00`, extremos exclusivos). El lector
retenía solo la última, que no contiene los destinos conocidos del adaptador,
y rechazaba el layout. El fixture inicial solo tenía una; añadir la segunda
reprodujo RED conductual: `Compatible image bounds rejected` en
`duplicate-text-red.log`. La corrección conserva hasta 96 rangos acotados y
elige como rango principal el que contiene ambos destinos conocidos. El test
exige también admitir código de la segunda sección y rechazar el hueco.

Evidencia privada: `evidence/context-forwarding-v24-3228-loader.json`,
`evidence/context-forwarding-v24-3228-analysis.json`,
`evidence/context-forwarding-v24-3228-startup-verification.json` y
`out/context-forwarding-20261018/diagnostic/Release/live_pair_probe_v24_3228.jsonl`.
SHA-256 JSONL: `81c4f84d825806a87f04ad655757e4b10d33edff38596442b5e6eb1c56619572`.
El analizador rechaza la captura y declara el forwarding no observado.

### Corrección host v26

Build aislado `out/context-forwarding-20261018/corrected/Release/`, DLL
`ac6_live_pair_probe_v26.dll`, SHA-256
`ce1c2a82fc951919fc8ae7c202276ccc9754e557d8334d2eb14c0b1d8f16e40f`.
No se ha cargado al registrar esta corrección. v24 mapeada no se recompila ni
sobrescribe. Ocho CTests correctos con las tres opciones ON, incluido el RED
anterior ahora GREEN, y ocho OFF en build predeterminado nuevo con nombre v27.
La revisión independiente de la corrección no encontró problemas. El log de
fallo añade `hook_activation_attempted` y `hook_baselines_captured` para
distinguir rechazo de preparación de fallo después de activar. No cambia la
creación/desactivación ni relaja la comprobación conservadora de restauración.

### Única observación mínima pendiente

Tras comprobación actual de presencia y misión cooperativa offline: una
ventana de ocho segundos con v28, sin visor/consumidor/seguimiento. v26 terminó
normalmente, por lo que su PID puede reutilizarse si identidad,
prólogos y DLL nueva no mapeada pasan de nuevo el loader. Si hay un fallo o
segunda cola, detener y conservar evidencia, sin recargar ni relajar gates.

Éxito: captura normal, nueve hooks restaurados y juego respondiendo; al menos
un `forwarding.status:candidate` completo dentro de su invocación CPU, con
parent, payload, hija, vtable y destino slot8 no nulos. Conservar JSON del
loader, JSONL completo y análisis, también si solo hay rechazos. Eso permitirá
analizar el destino concreto sin seguir punteros caducados; no certificará
proyección por frame ni validación PSVR2.

## Observación presencial v26 — PID 3228

Confirmación expresa: «Sí, listo para los 8 segundos con v26». El loader volvió
a verificar identidad, cooperativo, nombre nuevo y prólogo; código de salida 0.
Una sola ventana sin visor/consumidor/seguimiento ni screenshots auxiliares.

| Observación | Resultado |
| --- | --- |
| Captura | 3 pares, 716 descartes por backpressure, una cola DIRECT; sin fallo ni ledger inseguro |
| Traza | 130 listas, cierre en envío 111, cero overflow/pares omitidos; procedencia de captura completa |
| Contexto/helper | cero invocaciones del contexto, cero muestras admitidas del helper; 143202 llamadas sin token omitidas |
| Forwarding | cero candidatos y cero rechazos de layout; no hubo snapshot de una interfaz hija |
| Cierre | nueve hooks restaurados, DLL retenida inactiva y juego respondiendo |

Se verificó en juego que la corrección de secciones supera la preparación y
permite la captura normal. **No** se verificó el lector de interfaz hija en
juego: no recibió ninguna invocación dentro de su ventana. La ausencia de
registros tampoco demuestra una interfaz nula ni un tipo incompatible.

El log v26 no conserva si `BeginWindow` se ejecutó. La lectura del código
establece que `Armed` permanece durante la captura continua, así que no se
atribuye el cero a una supuesta transición rápida hacia otro estado. No se ha
aislado su causa ni probado que ese punto se ejecute en cada frame/estado.
No se repitió ninguna carga tras v26.

Evidencia privada: `evidence/context-forwarding-v26-3228-loader.json`,
`evidence/context-forwarding-v26-3228-analysis.json` (analizador inicial),
`evidence/context-forwarding-v26-3228-analysis-window.json` (resultado explícito
de cobertura), `evidence/context-forwarding-v26-3228-observation.json` y
`out/context-forwarding-20261018/corrected/Release/live_pair_probe_v26_3228.jsonl`.
SHA-256 JSONL: `5e360791a51d391e89455bbf53510b4645a98710a96a194717f6bb92f8513b5f`.
Ambos análisis conservan `capture_provenance_complete:true` y
`frame_associated:false`; el segundo clasifica el forwarding `no_coverage`.

### Cobertura vacía y apertura de ventana — host v28

El analizador inicial decía `context_forwarding_status:complete` para una
publicación vacía. Solo acreditaba integridad de publicación, sin candidatos,
pero no hacía explícita la ausencia de cobertura. Un fixture reproduce RED
ese resultado ambiguo y ahora exige `no_coverage` incluso con captura válida.

El observador conserva `window_started` en su resumen: inicialmente true solo
si el constructor abre la ventana; tras `BeginWindow` exitoso permanece true
después de Stop. Es estado del control thread, sin nuevas lecturas/contadores
en el callback. Python exige bool y rechaza false con invocaciones publicadas.
Los logs anteriores devuelven `context_forwarding_window_started:null`:
desconocido, sin reinterpretarlo como false ni inferirlo de timestamps.

Build aislado `out/context-forwarding-20261018/window/Release/`, DLL nueva
`ac6_live_pair_probe_v28.dll`, SHA-256
`c97803093fdbd9b71f3c60fa24bea8a1a866c8ffd7a2d802a763338c34f29bf5`.
**No cargada en AC6**. Ocho CTests correctos y suite Python de 68: 67 correctas,
una integración GPU opcional omitida. La revisión independiente no encontró
problemas. Las DLLs v24/v26 mapeadas se conservan intactas.

La única próxima observación es la descrita arriba con v28, en otra ventana
presencial, sin repetir v26. Además de las condiciones de éxito anteriores,
debe registrar `window_started:true`. Si no hay invocaciones, conservar
`no_coverage`: una ventana abierta y vacía permitirá acotar la investigación
a la ejecución/cobertura del punto, sin seleccionar otra matriz por cercanía.

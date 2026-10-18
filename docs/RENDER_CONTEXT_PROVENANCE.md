# Del helper de proyección al contexto de render — 18-10-2026

Continuación desde `95467e8`, después de la [observación v18](FRAME_PROJECTION_PROVENANCE.md#verificación-presencial-v18--pid-11284).
El incremento se probó primero en host. La pasada posterior v20 se registra
debajo: activó correctamente la parada por segunda cola y no certifica la
captura. No se ha probado presentación en PSVR2.

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
`out/render-context-20261018/scoped` / DLL v22. Este build **no se ha cargado**.
Los binarios v17/v18/v20 mapeados permanecen intactos e inactivos.

## Única observación mínima pendiente

Después de la parada por segunda cola, requiere una sesión/PID nuevos,
presencia del usuario y confirmación actual de misión cooperativa offline.
Ocho segundos sin visor/consumidor/seguimiento, girando suavemente
con mando. El loader debe volver a comprobar ejecutable/hash, módulos y nombre
de DLL nuevo/no mapeado. No recargar v17/v18, no usar matchmaking y no iniciar
AC6/SteamVR automáticamente. Si cambia la identidad, se rechaza el ensayo.

Usar la DLL v22 recién construida, solo tras la confirmación presencial:

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

La observación permitiría inspeccionar la implementación virtual concreta para
seguir el upload/binding. Seguirían pendientes el rango/versión de bytes usados
por GPU, el draw que escribe el depth capturado, viewport y la correspondencia
con TAA/color final. Esta sonda no supera por sí sola el gate de proyección.
